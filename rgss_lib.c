#define _POSIX_C_SOURCE 200809L
#include "rgss_lib.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <io.h>
    #include <windows.h>
    #define PATH_SEP '\\'
    #define IS_ATTY(fd) (_isatty(fd) != 0)
    #define FILENO(stream) _fileno(stream)
#else
    #include <unistd.h>
    #define PATH_SEP '/'
    #define IS_ATTY(fd) (isatty(fd) != 0)
    #define FILENO(stream) fileno(stream)
#endif

#define RGSS_MAGIC "RGSSAD\0"
#define RGSS_MAGIC_LEN 7
#define RGSS_V1_INITIAL_KEY 0xDEADCAFEu
#define RGSS_V3_DEFAULT_SEED 0x03975A21u
#define IO_BUFFER_SIZE (16 * 1024)
#define MAX_SCAN_PATH 4096

TermColors g_colors = {
    .reset        = "",
    .bold         = "",
    .err          = "",
    .ok           = "",
    .warn         = "",
    .accent       = "",
    .dir          = "",
    .file         = "",
    .muted        = "",
    .err_stderr   = "",
    .reset_stderr = ""
};

static bool enable_vt_mode(int fd) {
#ifdef _WIN32
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
#else
    (void)fd;
    return true;
#endif
}

void rgss_init_colors(void) {
    const char* no_color = getenv("NO_COLOR");
    if (no_color && no_color[0] != '\0') return;

    const char* term = getenv("TERM");
    if (term && strcmp(term, "dumb") == 0) return;

    int out_fd = FILENO(stdout);
    int err_fd = FILENO(stderr);

    if (IS_ATTY(out_fd) && enable_vt_mode(out_fd)) {
        g_colors.reset  = "\x1B[0m";
        g_colors.bold   = "\x1B[1m";
        g_colors.err    = "\x1B[91m";
        g_colors.ok     = "\x1B[92m";
        g_colors.warn   = "\x1B[93m";
        g_colors.accent = "\x1B[94m";
        g_colors.dir    = "\x1B[36m";
        g_colors.file   = "\x1B[97m";
        g_colors.muted  = "\x1B[90m";
    }

    if (IS_ATTY(err_fd) && enable_vt_mode(err_fd)) {
        g_colors.err_stderr   = "\x1B[91m";
        g_colors.reset_stderr = "\x1B[0m";
    }
}

void rgss_format_highlighted_name(const char* name, int pad_width, char* out_buf, size_t out_size) {
    const char* last_sep = strrchr(name, '\\');
    const char* last_fsep = strrchr(name, '/');
    if (last_fsep && (!last_sep || last_fsep > last_sep)) {
        last_sep = last_fsep;
    }

    size_t raw_len = strlen(name);
    int rem = (pad_width > (int)raw_len) ? (pad_width - (int)raw_len) : 0;

    if (last_sep) {
        size_t dir_len = (size_t)(last_sep - name + 1);
        snprintf(out_buf, out_size, "%s%.*s%s%s%*s%s",
                 g_colors.dir, (int)dir_len, name,
                 g_colors.file, last_sep + 1,
                 rem, "", g_colors.reset);
    } else {
        snprintf(out_buf, out_size, "%s%s%*s%s",
                 g_colors.file, name,
                 rem, "", g_colors.reset);
    }
}

static bool join_paths_safe(char* dst, size_t dst_size, const char* a, const char* b) {
    if (!a || !b || dst_size == 0) return false;
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    bool need_sep = (len_a > 0 && a[len_a - 1] != '/' && a[len_a - 1] != '\\');
    size_t total = len_a + (need_sep ? 1 : 0) + len_b;
    if (total >= dst_size) return false;

    memcpy(dst, a, len_a);
    size_t pos = len_a;
    if (need_sep) {
        dst[pos++] = PATH_SEP;
    }
    memcpy(dst + pos, b, len_b);
    dst[pos + len_b] = '\0';
    return true;
}

static bool read_u32_le(FILE* f, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

static bool write_u32_le(FILE* f, uint32_t val) {
    uint8_t b[4];
    b[0] = (uint8_t)(val & 0xFFu);
    b[1] = (uint8_t)((val >> 8) & 0xFFu);
    b[2] = (uint8_t)((val >> 16) & 0xFFu);
    b[3] = (uint8_t)((val >> 24) & 0xFFu);
    return fwrite(b, 1, 4, f) == 4;
}

static bool read_u8(FILE* f, uint8_t* out) {
    return fread(out, 1, 1, f) == 1;
}

static bool write_u8(FILE* f, uint8_t val) {
    return fwrite(&val, 1, 1, f) == 1;
}

static bool seek_u64(FILE* f, uint64_t offset, int origin) {
#ifdef _WIN32
    return _fseeki64(f, (__int64)offset, origin) == 0;
#else
    return fseeko(f, (off_t)offset, origin) == 0;
#endif
}

static uint64_t tell_u64(FILE* f) {
#ifdef _WIN32
    __int64 pos = _ftelli64(f);
    return (pos < 0) ? 0 : (uint64_t)pos;
#else
    off_t pos = ftello(f);
    return (pos < 0) ? 0 : (uint64_t)pos;
#endif
}

static inline uint32_t step_key(uint32_t* key) {
    uint32_t current = *key;
    *key = (*key * 7u) + 3u;
    return current;
}

static char* duplicate_string(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1);
    if (copy) {
        memcpy(copy, s, len + 1);
    }
    return copy;
}

static bool archive_add_file(RgssArchive* archive, RgssFilePointer fp) {
    if (archive->file_count >= archive->file_capacity) {
        size_t new_cap = (archive->file_capacity == 0) ? 64 : archive->file_capacity * 2;
        RgssFilePointer* resized = (RgssFilePointer*)realloc(archive->files, new_cap * sizeof(RgssFilePointer));
        if (!resized) return false;
        archive->files = resized;
        archive->file_capacity = new_cap;
    }
    archive->files[archive->file_count++] = fp;
    return true;
}

int rgss_get_version(const char* path) {
    if (!path || path[0] == '\0') return -1;

    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    uint8_t header[8];
    size_t read = fread(header, 1, sizeof(header), f);
    fclose(f);

    if (read < sizeof(header)) return -1;
    if (memcmp(header, RGSS_MAGIC, RGSS_MAGIC_LEN) != 0) return -1;

    return (int)header[7];
}

static bool parse_v1(RgssArchive* archive, FILE* f, uint64_t file_size, char* err_msg, size_t err_size) {
    uint32_t key = RGSS_V1_INITIAL_KEY;

    while (tell_u64(f) < file_size) {
        uint32_t name_len = 0;
        if (!read_u32_le(f, &name_len)) {
            snprintf(err_msg, err_size, "Unexpected EOF reading filename length");
            return false;
        }
        name_len ^= step_key(&key);

        if (name_len == 0 || name_len > 4096) {
            snprintf(err_msg, err_size, "Invalid filename length (%u)", name_len);
            return false;
        }

        char* name = (char*)malloc(name_len + 1);
        if (!name) {
            snprintf(err_msg, err_size, "Out of memory");
            return false;
        }

        for (uint32_t i = 0; i < name_len; i++) {
            uint8_t ch = 0;
            if (!read_u8(f, &ch)) {
                free(name);
                snprintf(err_msg, err_size, "Unexpected EOF reading filename");
                return false;
            }
            uint32_t k = step_key(&key);
            name[i] = (char)(ch ^ (uint8_t)(k & 0xFFu));
        }
        name[name_len] = '\0';

        uint32_t data_size = 0;
        if (!read_u32_le(f, &data_size)) {
            snprintf(err_msg, err_size, "Unexpected EOF reading entry size for %.256s", name);
            free(name);
            return false;
        }
        data_size ^= step_key(&key);

        uint64_t data_offset = tell_u64(f);
        if (data_offset + data_size > file_size) {
            snprintf(err_msg, err_size, "Entry %.256s exceeds archive bounds", name);
            free(name);
            return false;
        }

        RgssFilePointer fp = {
            .name = name,
            .offset = data_offset,
            .size = data_size,
            .key = key
        };

        if (!archive_add_file(archive, fp)) {
            free(name);
            snprintf(err_msg, err_size, "Out of memory");
            return false;
        }

        if (!seek_u64(f, data_offset + data_size, SEEK_SET)) {
            snprintf(err_msg, err_size, "Failed to seek past entry %.256s", name);
            return false;
        }
    }

    return true;
}

static bool parse_v3(RgssArchive* archive, FILE* f, uint64_t file_size, char* err_msg, size_t err_size) {
    uint32_t base_key = 0;
    if (!read_u32_le(f, &base_key)) {
        snprintf(err_msg, err_size, "Unexpected EOF reading RGSS3 base key");
        return false;
    }
    base_key = (base_key * 9u) + 3u;

    while (tell_u64(f) < file_size) {
        uint32_t offset = 0, size = 0, entry_key = 0, name_len = 0;

        if (!read_u32_le(f, &offset) ||
            !read_u32_le(f, &size) ||
            !read_u32_le(f, &entry_key) ||
            !read_u32_le(f, &name_len)) {
            snprintf(err_msg, err_size, "Unexpected EOF reading RGSS3 file table entry");
            return false;
        }

        offset ^= base_key;
        if (offset == 0) {
            break;
        }

        size ^= base_key;
        entry_key ^= base_key;
        name_len ^= base_key;

        if (name_len == 0 || name_len > 4096) {
            snprintf(err_msg, err_size, "Invalid RGSS3 filename length (%u)", name_len);
            return false;
        }

        char* name = (char*)malloc(name_len + 1);
        if (!name) {
            snprintf(err_msg, err_size, "Out of memory");
            return false;
        }

        for (uint32_t i = 0; i < name_len; i++) {
            uint8_t ch = 0;
            if (!read_u8(f, &ch)) {
                free(name);
                snprintf(err_msg, err_size, "Unexpected EOF reading RGSS3 filename");
                return false;
            }
            uint8_t key_byte = (uint8_t)((base_key >> ((i % 4) * 8)) & 0xFFu);
            name[i] = (char)(ch ^ key_byte);
        }
        name[name_len] = '\0';

        if ((uint64_t)offset + (uint64_t)size > file_size) {
            snprintf(err_msg, err_size, "Entry %.256s exceeds archive bounds", name);
            free(name);
            return false;
        }

        RgssFilePointer fp = {
            .name = name,
            .offset = (uint64_t)offset,
            .size = size,
            .key = entry_key
        };

        if (!archive_add_file(archive, fp)) {
            free(name);
            snprintf(err_msg, err_size, "Out of memory");
            return false;
        }
    }

    return true;
}

bool rgss_archive_open(RgssArchive* archive, const char* path, int version, char* err_msg, size_t err_size) {
    memset(archive, 0, sizeof(*archive));
    archive->version = version;
    archive->data_path = duplicate_string(path);
    if (!archive->data_path) {
        snprintf(err_msg, err_size, "Out of memory");
        return false;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(err_msg, err_size, "Cannot open file: %s", strerror(errno));
        rgss_archive_free(archive);
        return false;
    }

    if (!seek_u64(f, 0, SEEK_END)) {
        fclose(f);
        snprintf(err_msg, err_size, "Cannot seek archive");
        rgss_archive_free(archive);
        return false;
    }
    uint64_t file_size = tell_u64(f);

    if (file_size < 8 || !seek_u64(f, 8, SEEK_SET)) {
        fclose(f);
        snprintf(err_msg, err_size, "Archive header is truncated");
        rgss_archive_free(archive);
        return false;
    }

    bool ok = false;
    if (version == 1) {
        ok = parse_v1(archive, f, file_size, err_msg, err_size);
    } else if (version == 3) {
        ok = parse_v3(archive, f, file_size, err_msg, err_size);
    } else {
        snprintf(err_msg, err_size, "Unsupported RGSS version %d", version);
    }

    fclose(f);
    if (!ok) {
        rgss_archive_free(archive);
    }
    return ok;
}

static bool stream_xor_copy(FILE* in, FILE* out, uint32_t size, uint32_t initial_key,
                            size_t* out_processed, char* err_msg, size_t err_size) {
    uint8_t buffer[IO_BUFFER_SIZE];
    uint32_t key = initial_key;
    uint32_t current_mask = step_key(&key);
    int byte_lane = 0;
    size_t total = 0;

    while (total < size) {
        size_t remaining = (size_t)size - total;
        size_t to_read = (remaining < sizeof(buffer)) ? remaining : sizeof(buffer);
        size_t n = fread(buffer, 1, to_read, in);

        if (n == 0) {
            break;
        }

        for (size_t i = 0; i < n; i++) {
            buffer[i] ^= (uint8_t)((current_mask >> (byte_lane * 8)) & 0xFFu);
            byte_lane++;
            if (byte_lane == 4) {
                byte_lane = 0;
                current_mask = step_key(&key);
            }
        }

        if (fwrite(buffer, 1, n, out) != n) {
            snprintf(err_msg, err_size, "Write error: %s", strerror(errno));
            return false;
        }

        total += n;
    }

    if (out_processed) *out_processed = total;
    return true;
}

bool rgss_extract_file_to_disk(const RgssArchive* archive, const RgssFilePointer* fp,
                               const char* target_path, char* err_msg, size_t err_size) {
    FILE* in = fopen(archive->data_path, "rb");
    if (!in) {
        snprintf(err_msg, err_size, "Failed to open archive: %s", strerror(errno));
        return false;
    }

    if (!seek_u64(in, fp->offset, SEEK_SET)) {
        fclose(in);
        snprintf(err_msg, err_size, "Failed to seek to %.256s", fp->name);
        return false;
    }

    FILE* out = fopen(target_path, "wb");
    if (!out) {
        fclose(in);
        snprintf(err_msg, err_size, "Failed to create %.256s: %s", target_path, strerror(errno));
        return false;
    }

    size_t total_read = 0;
    bool ok = stream_xor_copy(in, out, fp->size, fp->key, &total_read, err_msg, err_size);

    fclose(in);
    fclose(out);

    if (!ok) return false;

    if (total_read < fp->size) {
        snprintf(err_msg, err_size, "Incomplete file %.256s (%zu/%u bytes)",
                 fp->name, total_read, fp->size);
        return false;
    }

    return true;
}

// Archive Packing Implementation

typedef struct {
    char* disk_path;
    char* archive_name;
    uint32_t size;
    uint32_t key;
    uint64_t offset;
} PackEntry;

typedef struct {
    PackEntry* items;
    size_t count;
    size_t capacity;
} PackList;

static void pack_list_free(PackList* list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i].disk_path);
        free(list->items[i].archive_name);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static bool pack_list_add(PackList* list, const char* disk_path, const char* archive_name, uint32_t size) {
    if (list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 64 : list->capacity * 2;
        PackEntry* resized = (PackEntry*)realloc(list->items, new_cap * sizeof(PackEntry));
        if (!resized) return false;
        list->items = resized;
        list->capacity = new_cap;
    }
    PackEntry entry = {
        .disk_path = duplicate_string(disk_path),
        .archive_name = duplicate_string(archive_name),
        .size = size,
        .key = 0,
        .offset = 0
    };
    if (!entry.disk_path || !entry.archive_name) {
        free(entry.disk_path);
        free(entry.archive_name);
        return false;
    }
    list->items[list->count++] = entry;
    return true;
}

static int compare_pack_entries(const void* a, const void* b) {
    const PackEntry* pa = (const PackEntry*)a;
    const PackEntry* pb = (const PackEntry*)b;
    return strcmp(pa->archive_name, pb->archive_name);
}

static bool is_ignored_pack_file(const char* name) {
    if (name[0] == '.') return true;
    if (strcmp(name, "Game.rxproj") == 0 ||
        strcmp(name, "Game.rvproj") == 0 ||
        strcmp(name, "Game.rvproj2") == 0) {
        return true;
    }
    const char* dot = strrchr(name, '.');
    if (dot) {
        char ext[16] = {0};
        for (size_t i = 0; dot[i] && i < sizeof(ext) - 1; i++) {
            ext[i] = (char)tolower((unsigned char)dot[i]);
        }
        if (strcmp(ext, ".rgssad") == 0 ||
            strcmp(ext, ".rgss2a") == 0 ||
            strcmp(ext, ".rgss3a") == 0) {
            return true;
        }
    }
    return false;
}

static bool scan_directory_recursive(const char* base_disk, const char* rel_prefix,
                                     PackList* list, char* err_msg, size_t err_size) {
    char current_disk[MAX_SCAN_PATH];
    if (rel_prefix[0] == '\0') {
        snprintf(current_disk, sizeof(current_disk), "%s", base_disk);
    } else if (!join_paths_safe(current_disk, sizeof(current_disk), base_disk, rel_prefix)) {
        snprintf(err_msg, err_size, "Directory path too long");
        return false;
    }

    DIR* dir = opendir(current_disk);
    if (!dir) {
        snprintf(err_msg, err_size, "Cannot open directory %.256s: %s", current_disk, strerror(errno));
        return false;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (is_ignored_pack_file(ent->d_name)) {
            continue;
        }

        char next_disk[MAX_SCAN_PATH];
        char next_rel[MAX_SCAN_PATH];
        if (!join_paths_safe(next_disk, sizeof(next_disk), current_disk, ent->d_name)) {
            closedir(dir);
            snprintf(err_msg, err_size, "File path exceeds maximum length");
            return false;
        }

        if (rel_prefix[0] == '\0') {
            snprintf(next_rel, sizeof(next_rel), "%s", ent->d_name);
        } else if (!join_paths_safe(next_rel, sizeof(next_rel), rel_prefix, ent->d_name)) {
            closedir(dir);
            snprintf(err_msg, err_size, "Relative path exceeds maximum length");
            return false;
        }

        struct stat st;
        if (stat(next_disk, &st) != 0) {
            closedir(dir);
            snprintf(err_msg, err_size, "Cannot stat %.256s: %s", next_disk, strerror(errno));
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!scan_directory_recursive(base_disk, next_rel, list, err_msg, err_size)) {
                closedir(dir);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if ((uint64_t)st.st_size > UINT32_MAX) {
                closedir(dir);
                snprintf(err_msg, err_size, "File %.256s exceeds 4GB RGSS limit", next_rel);
                return false;
            }
            for (char* p = next_rel; *p; p++) {
                if (*p == '/') *p = '\\';
            }
            if (!pack_list_add(list, next_disk, next_rel, (uint32_t)st.st_size)) {
                closedir(dir);
                snprintf(err_msg, err_size, "Out of memory");
                return false;
            }
        }
    }

    closedir(dir);
    return true;
}

static int detect_target_version(const char* out_archive_path) {
    const char* dot = strrchr(out_archive_path, '.');
    if (!dot) return 3;

    char ext[16] = {0};
    for (size_t i = 0; dot[i] && i < sizeof(ext) - 1; i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    if (strcmp(ext, ".rgssad") == 0 || strcmp(ext, ".rgss2a") == 0) {
        return 1;
    }
    if (strcmp(ext, ".rgss3a") == 0) {
        return 3;
    }
    return -1;
}

static bool write_packed_v1(FILE* out, PackList* list, bool suppress, char* err_msg, size_t err_size) {
    uint32_t key = RGSS_V1_INITIAL_KEY;
    int digits = (list->count > 0) ? (int)floor(log10((double)list->count) + 1) : 1;

    for (size_t i = 0; i < list->count; i++) {
        PackEntry* entry = &list->items[i];
        uint32_t name_len = (uint32_t)strlen(entry->archive_name);

        if (!suppress) {
            char name_highlight[MAX_SCAN_PATH + 64];
            rgss_format_highlighted_name(entry->archive_name, 0, name_highlight, sizeof(name_highlight));
            printf("%s[%*zu/%zu] Pack     :%s %s\n",
                   g_colors.ok, digits, i + 1, list->count, g_colors.reset, name_highlight);
        }

        if (!write_u32_le(out, name_len ^ step_key(&key))) {
            snprintf(err_msg, err_size, "Failed writing entry name length");
            return false;
        }

        for (uint32_t j = 0; j < name_len; j++) {
            uint32_t k = step_key(&key);
            uint8_t enc = (uint8_t)entry->archive_name[j] ^ (uint8_t)(k & 0xFFu);
            if (!write_u8(out, enc)) {
                snprintf(err_msg, err_size, "Failed writing entry name");
                return false;
            }
        }

        if (!write_u32_le(out, entry->size ^ step_key(&key))) {
            snprintf(err_msg, err_size, "Failed writing entry size");
            return false;
        }

        FILE* in = fopen(entry->disk_path, "rb");
        if (!in) {
            snprintf(err_msg, err_size, "Cannot read %.256s: %s", entry->disk_path, strerror(errno));
            return false;
        }

        size_t written = 0;
        bool ok = stream_xor_copy(in, out, entry->size, key, &written, err_msg, err_size);
        fclose(in);

        if (!ok || written != entry->size) {
            if (ok) snprintf(err_msg, err_size, "Short read packing %.256s", entry->archive_name);
            return false;
        }
    }

    return true;
}

static bool write_packed_v3(FILE* out, PackList* list, bool suppress, char* err_msg, size_t err_size) {
    uint32_t seed = RGSS_V3_DEFAULT_SEED;
    uint32_t base_key = (seed * 9u) + 3u;
    int digits = (list->count > 0) ? (int)floor(log10((double)list->count) + 1) : 1;

    if (!write_u32_le(out, seed)) {
        snprintf(err_msg, err_size, "Failed writing RGSS3 seed");
        return false;
    }

    uint64_t table_size = 0;
    for (size_t i = 0; i < list->count; i++) {
        table_size += 16u + (uint64_t)strlen(list->items[i].archive_name);
    }
    table_size += 16u;

    uint64_t current_data_offset = 12u + table_size;
    if (current_data_offset > UINT32_MAX) {
        snprintf(err_msg, err_size, "Archive exceeds 4GB RGSS3 offset limit");
        return false;
    }

    for (size_t i = 0; i < list->count; i++) {
        PackEntry* entry = &list->items[i];
        uint32_t name_len = (uint32_t)strlen(entry->archive_name);
        entry->offset = current_data_offset;
        entry->key = (uint32_t)((i + 1u) * 0x1337u + 0x55AA55AAu);

        if (!write_u32_le(out, (uint32_t)entry->offset ^ base_key) ||
            !write_u32_le(out, entry->size ^ base_key) ||
            !write_u32_le(out, entry->key ^ base_key) ||
            !write_u32_le(out, name_len ^ base_key)) {
            snprintf(err_msg, err_size, "Failed writing RGSS3 table header");
            return false;
        }

        for (uint32_t j = 0; j < name_len; j++) {
            uint8_t key_byte = (uint8_t)((base_key >> ((j % 4) * 8)) & 0xFFu);
            uint8_t enc = (uint8_t)entry->archive_name[j] ^ key_byte;
            if (!write_u8(out, enc)) {
                snprintf(err_msg, err_size, "Failed writing RGSS3 entry name");
                return false;
            }
        }

        current_data_offset += entry->size;
        if (current_data_offset > UINT32_MAX) {
            snprintf(err_msg, err_size, "Archive exceeds 4GB RGSS3 size limit");
            return false;
        }
    }

    if (!write_u32_le(out, 0u ^ base_key) ||
        !write_u32_le(out, 0u) ||
        !write_u32_le(out, 0u) ||
        !write_u32_le(out, 0u)) {
        snprintf(err_msg, err_size, "Failed writing RGSS3 table terminator");
        return false;
    }

    for (size_t i = 0; i < list->count; i++) {
        PackEntry* entry = &list->items[i];
        if (!suppress) {
            char name_highlight[MAX_SCAN_PATH + 64];
            rgss_format_highlighted_name(entry->archive_name, 0, name_highlight, sizeof(name_highlight));
            printf("%s[%*zu/%zu] Pack     :%s %s\n",
                   g_colors.ok, digits, i + 1, list->count, g_colors.reset, name_highlight);
        }

        FILE* in = fopen(entry->disk_path, "rb");
        if (!in) {
            snprintf(err_msg, err_size, "Cannot read %.256s: %s", entry->disk_path, strerror(errno));
            return false;
        }

        size_t written = 0;
        bool ok = stream_xor_copy(in, out, entry->size, entry->key, &written, err_msg, err_size);
        fclose(in);

        if (!ok || written != entry->size) {
            if (ok) snprintf(err_msg, err_size, "Short read packing %.256s", entry->archive_name);
            return false;
        }
    }

    return true;
}

bool rgss_archive_pack(const char* source_dir, const char* out_archive_path, bool suppress,
                       char* err_msg, size_t err_size) {
    int version = detect_target_version(out_archive_path);
    if (version == -1) {
        snprintf(err_msg, err_size, "Output archive must end with .rgssad, .rgss2a, or .rgss3a");
        return false;
    }

    PackList list = {0};
    if (!scan_directory_recursive(source_dir, "", &list, err_msg, err_size)) {
        pack_list_free(&list);
        return false;
    }

    if (list.count == 0) {
        pack_list_free(&list);
        snprintf(err_msg, err_size, "No packable files found in %.256s", source_dir);
        return false;
    }

    qsort(list.items, list.count, sizeof(PackEntry), compare_pack_entries);

    printf("%s------%s\n", g_colors.muted, g_colors.reset);
    printf("Packing %s%zu%s Files (RGSS v%d)\n", g_colors.bold, list.count, g_colors.reset, version);
    printf("Output Archive: %s%s%s\n", g_colors.dir, out_archive_path, g_colors.reset);
    printf("%s------%s\n", g_colors.muted, g_colors.reset);

    FILE* out = fopen(out_archive_path, "wb");
    if (!out) {
        pack_list_free(&list);
        snprintf(err_msg, err_size, "Cannot create %.256s: %s", out_archive_path, strerror(errno));
        return false;
    }

    uint8_t header[8] = { 'R', 'G', 'S', 'S', 'A', 'D', 0, (uint8_t)version };
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fclose(out);
        pack_list_free(&list);
        snprintf(err_msg, err_size, "Failed writing RGSS header");
        return false;
    }

    bool ok = (version == 1)
        ? write_packed_v1(out, &list, suppress, err_msg, err_size)
        : write_packed_v3(out, &list, suppress, err_msg, err_size);

    fclose(out);
    pack_list_free(&list);
    return ok;
}

void rgss_archive_free(RgssArchive* archive) {
    if (!archive) return;
    free(archive->data_path);
    if (archive->files) {
        for (size_t i = 0; i < archive->file_count; i++) {
            free(archive->files[i].name);
        }
        free(archive->files);
    }
    memset(archive, 0, sizeof(*archive));
}
