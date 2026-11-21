#define _POSIX_C_SOURCE 200809L
#include "rgss_lib.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #include <windows.h>
    #define PATH_SEP '\\'
#else
    #include <dirent.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #define PATH_SEP '/'
#endif

#define RGSS_MAGIC "RGSSAD\0"
#define RGSS_MAGIC_LEN 7
#define RGSS_V1_INITIAL_KEY 0xDEADCAFEu
#define IO_BUFFER_SIZE (16 * 1024)
#define MAX_PACK_PATH 4096

static bool join_paths(char* dst, size_t dst_size, const char* dir, const char* leaf) {
    size_t dlen = strlen(dir);
    size_t llen = strlen(leaf);

    while (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\')) {
        dlen--;
    }

    if (dlen + 1 + llen + 1 > dst_size) {
        return false;
    }

    memcpy(dst, dir, dlen);
    dst[dlen] = PATH_SEP;
    memcpy(dst + dlen + 1, leaf, llen);
    dst[dlen + 1 + llen] = '\0';
    return true;
}

static bool join_archive_rel(char* dst, size_t dst_size, const char* dir, const char* leaf) {
    if (!dir || dir[0] == '\0') {
        size_t llen = strlen(leaf);
        if (llen + 1 > dst_size) return false;
        memcpy(dst, leaf, llen + 1);
        return true;
    }
    size_t dlen = strlen(dir);
    size_t llen = strlen(leaf);
    if (dlen + 1 + llen + 1 > dst_size) return false;

    memcpy(dst, dir, dlen);
    dst[dlen] = '\\';
    memcpy(dst + dlen + 1, leaf, llen);
    dst[dlen + 1 + llen] = '\0';
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
    uint8_t b[4] = {
        (uint8_t)(val & 0xFFu),
        (uint8_t)((val >> 8) & 0xFFu),
        (uint8_t)((val >> 16) & 0xFFu),
        (uint8_t)((val >> 24) & 0xFFu)
    };
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
        snprintf(err_msg, err_size, "Cannot open file: %.256s", strerror(errno));
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

static bool write_encrypted_stream(FILE* in, FILE* out, uint32_t initial_key, uint32_t expected_size,
                                   size_t* out_written, char* err_msg, size_t err_size) {
    uint8_t buffer[IO_BUFFER_SIZE];
    uint32_t key = initial_key;
    uint32_t current_mask = step_key(&key);
    int byte_lane = 0;
    size_t total = 0;

    while (total < expected_size) {
        size_t remaining = (size_t)expected_size - total;
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
            snprintf(err_msg, err_size, "Write error: %.256s", strerror(errno));
            return false;
        }

        total += n;
    }

    if (out_written) *out_written = total;
    return true;
}

bool rgss_extract_file_to_disk(const RgssArchive* archive, const RgssFilePointer* fp,
                               const char* target_path, char* err_msg, size_t err_size) {
    FILE* in = fopen(archive->data_path, "rb");
    if (!in) {
        snprintf(err_msg, err_size, "Failed to open archive: %.256s", strerror(errno));
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
        snprintf(err_msg, err_size, "Failed to create %.256s: %.128s", target_path, strerror(errno));
        return false;
    }

    size_t total_read = 0;
    bool ok = write_encrypted_stream(in, out, fp->key, fp->size, &total_read, err_msg, err_size);

    fclose(in);
    fclose(out);

    if (!ok) {
        return false;
    }

    if (total_read < fp->size) {
        snprintf(err_msg, err_size, "Incomplete file %.256s (%zu/%u bytes)",
                 fp->name, total_read, fp->size);
        return false;
    }

    return true;
}

// --- Archive Repacking Implementation ---

typedef struct {
    char* disk_path;
    char* archive_name;
    uint32_t size;
    uint32_t key;
    uint32_t offset;
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

static bool pack_list_add(PackList* list, PackEntry entry) {
    if (list->count >= list->capacity) {
        size_t new_cap = (list->capacity == 0) ? 64 : list->capacity * 2;
        PackEntry* resized = (PackEntry*)realloc(list->items, new_cap * sizeof(PackEntry));
        if (!resized) return false;
        list->items = resized;
        list->capacity = new_cap;
    }
    list->items[list->count++] = entry;
    return true;
}

static int compare_pack_entries(const void* a, const void* b) {
    const PackEntry* ea = (const PackEntry*)a;
    const PackEntry* eb = (const PackEntry*)b;
    return strcmp(ea->archive_name, eb->archive_name);
}

static bool is_excluded_file(const char* name, const char* out_basename) {
    if (name[0] == '.') return true;
    if (out_basename && strcmp(name, out_basename) == 0) return true;
    if (strcmp(name, "Game.rxproj") == 0 ||
        strcmp(name, "Game.rvproj") == 0 ||
        strcmp(name, "Game.rvproj2") == 0) {
        return true;
    }
    return false;
}

static bool collect_files_recursive(const char* base_disk_dir, const char* rel_archive_dir,
                                    const char* out_basename, PackList* list,
                                    char* err_msg, size_t err_size) {
#ifdef _WIN32
    char search_path[MAX_PACK_PATH];
    if (!join_paths(search_path, sizeof(search_path), base_disk_dir, "*")) {
        snprintf(err_msg, err_size, "Directory path too long");
        return false;
    }

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        snprintf(err_msg, err_size, "Cannot open directory: %.256s", base_disk_dir);
        return false;
    }

    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (is_excluded_file(fd.cFileName, out_basename)) continue;

        char child_disk[MAX_PACK_PATH];
        char child_arch[MAX_PACK_PATH];
        if (!join_paths(child_disk, sizeof(child_disk), base_disk_dir, fd.cFileName) ||
            !join_archive_rel(child_arch, sizeof(child_arch), rel_archive_dir, fd.cFileName)) {
            FindClose(hFind);
            snprintf(err_msg, err_size, "Path exceeds maximum length");
            return false;
        }

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!collect_files_recursive(child_disk, child_arch, out_basename, list, err_msg, err_size)) {
                FindClose(hFind);
                return false;
            }
        } else {
            uint64_t fsize = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
            if (fsize > UINT32_MAX) {
                FindClose(hFind);
                snprintf(err_msg, err_size, "File exceeds 4GB RGSS limit: %.256s", child_arch);
                return false;
            }
            PackEntry entry = {
                .disk_path = duplicate_string(child_disk),
                .archive_name = duplicate_string(child_arch),
                .size = (uint32_t)fsize,
                .key = 0,
                .offset = 0
            };
            if (!entry.disk_path || !entry.archive_name || !pack_list_add(list, entry)) {
                free(entry.disk_path);
                free(entry.archive_name);
                FindClose(hFind);
                snprintf(err_msg, err_size, "Out of memory");
                return false;
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return true;
#else
    DIR* dir = opendir(base_disk_dir);
    if (!dir) {
        snprintf(err_msg, err_size, "Cannot open directory %.256s: %.128s", base_disk_dir, strerror(errno));
        return false;
    }

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (is_excluded_file(ent->d_name, out_basename)) continue;

        char child_disk[MAX_PACK_PATH];
        char child_arch[MAX_PACK_PATH];
        if (!join_paths(child_disk, sizeof(child_disk), base_disk_dir, ent->d_name) ||
            !join_archive_rel(child_arch, sizeof(child_arch), rel_archive_dir, ent->d_name)) {
            closedir(dir);
            snprintf(err_msg, err_size, "Path exceeds maximum length");
            return false;
        }

        struct stat st;
        if (stat(child_disk, &st) != 0) {
            closedir(dir);
            snprintf(err_msg, err_size, "Cannot stat %.256s: %.128s", child_disk, strerror(errno));
            return false;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!collect_files_recursive(child_disk, child_arch, out_basename, list, err_msg, err_size)) {
                closedir(dir);
                return false;
            }
        } else if (S_ISREG(st.st_mode)) {
            if ((uint64_t)st.st_size > UINT32_MAX) {
                closedir(dir);
                snprintf(err_msg, err_size, "File exceeds 4GB RGSS limit: %.256s", child_arch);
                return false;
            }
            PackEntry entry = {
                .disk_path = duplicate_string(child_disk),
                .archive_name = duplicate_string(child_arch),
                .size = (uint32_t)st.st_size,
                .key = 0,
                .offset = 0
            };
            if (!entry.disk_path || !entry.archive_name || !pack_list_add(list, entry)) {
                free(entry.disk_path);
                free(entry.archive_name);
                closedir(dir);
                snprintf(err_msg, err_size, "Out of memory");
                return false;
            }
        }
    }

    closedir(dir);
    return true;
#endif
}

static int detect_target_version(const char* out_archive) {
    const char* dot = strrchr(out_archive, '.');
    if (!dot) return -1;

    char ext[16] = {0};
    for (size_t i = 0; dot[i] && i < sizeof(ext) - 1; i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    if (strcmp(ext, ".rgssad") == 0 || strcmp(ext, ".rgss2a") == 0) return 1;
    if (strcmp(ext, ".rgss3a") == 0) return 3;
    return -1;
}

static bool write_pack_v1(FILE* out, PackList* list, bool suppress, char* err_msg, size_t err_size) {
    uint32_t key = RGSS_V1_INITIAL_KEY;

    for (size_t i = 0; i < list->count; i++) {
        PackEntry* e = &list->items[i];
        uint32_t name_len = (uint32_t)strlen(e->archive_name);

        if (!suppress) {
            printf("\x1B[32m[%zu/%zu] Packing  :\x1B[37m %s\x1B[0m\x1B[39m\n",
                   i + 1, list->count, e->archive_name);
        }

        if (!write_u32_le(out, name_len ^ step_key(&key))) {
            snprintf(err_msg, err_size, "Failed writing entry header");
            return false;
        }

        for (uint32_t j = 0; j < name_len; j++) {
            uint32_t k = step_key(&key);
            uint8_t enc_ch = (uint8_t)e->archive_name[j] ^ (uint8_t)(k & 0xFFu);
            if (!write_u8(out, enc_ch)) {
                snprintf(err_msg, err_size, "Failed writing entry filename");
                return false;
            }
        }

        if (!write_u32_le(out, e->size ^ step_key(&key))) {
            snprintf(err_msg, err_size, "Failed writing entry size");
            return false;
        }

        FILE* in = fopen(e->disk_path, "rb");
        if (!in) {
            snprintf(err_msg, err_size, "Cannot open %.256s: %.128s", e->disk_path, strerror(errno));
            return false;
        }

        size_t written = 0;
        bool ok = write_encrypted_stream(in, out, key, e->size, &written, err_msg, err_size);
        fclose(in);

        if (!ok || written < e->size) {
            if (ok) snprintf(err_msg, err_size, "Short read on %.256s", e->disk_path);
            return false;
        }
    }

    return true;
}

static bool write_pack_v3(FILE* out, PackList* list, bool suppress, char* err_msg, size_t err_size) {
    srand((unsigned int)time(NULL));
    uint32_t raw_seed = ((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ 0x13579BDFu;
    if (raw_seed == 0) raw_seed = 0x2468ACE0u;
    uint32_t base_key = (raw_seed * 9u) + 3u;

    if (!write_u32_le(out, raw_seed)) {
        snprintf(err_msg, err_size, "Failed writing RGSS3 seed");
        return false;
    }

    // Calculate exact file table size:
    // 8-byte header + 4-byte seed + sum(16 + name_len) + 16-byte null terminator
    uint64_t data_cursor = 8 + 4 + 16;
    for (size_t i = 0; i < list->count; i++) {
        data_cursor += 16 + strlen(list->items[i].archive_name);
    }

    if (data_cursor > UINT32_MAX) {
        snprintf(err_msg, err_size, "Archive file table exceeds 4GB RGSS3 offset limit");
        return false;
    }

    for (size_t i = 0; i < list->count; i++) {
        PackEntry* e = &list->items[i];
        if (data_cursor > UINT32_MAX) {
            snprintf(err_msg, err_size, "Archive exceeds 4GB RGSS3 offset limit");
            return false;
        }
        e->offset = (uint32_t)data_cursor;
        e->key = (((uint32_t)rand() << 16) ^ (uint32_t)rand() ^ (uint32_t)(i + 1) * 0x9E3779B9u) | 1u;
        data_cursor += e->size;

        uint32_t name_len = (uint32_t)strlen(e->archive_name);
        if (!write_u32_le(out, e->offset ^ base_key) ||
            !write_u32_le(out, e->size ^ base_key) ||
            !write_u32_le(out, e->key ^ base_key) ||
            !write_u32_le(out, name_len ^ base_key)) {
            snprintf(err_msg, err_size, "Failed writing RGSS3 table entry");
            return false;
        }

        for (uint32_t j = 0; j < name_len; j++) {
            uint8_t key_byte = (uint8_t)((base_key >> ((j % 4) * 8)) & 0xFFu);
            if (!write_u8(out, (uint8_t)e->archive_name[j] ^ key_byte)) {
                snprintf(err_msg, err_size, "Failed writing RGSS3 entry name");
                return false;
            }
        }
    }

    // Write table terminator (decrypted offset == 0)
    if (!write_u32_le(out, 0u ^ base_key) ||
        !write_u32_le(out, 0u) ||
        !write_u32_le(out, 0u) ||
        !write_u32_le(out, 0u)) {
        snprintf(err_msg, err_size, "Failed writing RGSS3 table terminator");
        return false;
    }

    // Write encrypted file payloads
    for (size_t i = 0; i < list->count; i++) {
        PackEntry* e = &list->items[i];
        if (!suppress) {
            printf("\x1B[32m[%zu/%zu] Packing  :\x1B[37m %s\x1B[0m\x1B[39m\n",
                   i + 1, list->count, e->archive_name);
        }

        FILE* in = fopen(e->disk_path, "rb");
        if (!in) {
            snprintf(err_msg, err_size, "Cannot open %.256s: %.128s", e->disk_path, strerror(errno));
            return false;
        }

        size_t written = 0;
        bool ok = write_encrypted_stream(in, out, e->key, e->size, &written, err_msg, err_size);
        fclose(in);

        if (!ok || written < e->size) {
            if (ok) snprintf(err_msg, err_size, "Short read on %.256s", e->disk_path);
            return false;
        }
    }

    return true;
}

bool rgss_archive_pack(const char* source_dir, const char* out_archive, bool suppress,
                       char* err_msg, size_t err_size) {
    int version = detect_target_version(out_archive);
    if (version == -1) {
        snprintf(err_msg, err_size, "Output extension must be .rgssad, .rgss2a, or .rgss3a");
        return false;
    }

    const char* out_base = strrchr(out_archive, '/');
    const char* out_bbase = strrchr(out_archive, '\\');
    if (out_bbase && (!out_base || out_bbase > out_base)) out_base = out_bbase;
    out_base = out_base ? (out_base + 1) : out_archive;

    PackList list = {0};
    if (!collect_files_recursive(source_dir, "", out_base, &list, err_msg, err_size)) {
        pack_list_free(&list);
        return false;
    }

    if (list.count == 0) {
        pack_list_free(&list);
        snprintf(err_msg, err_size, "No packable files found in %.256s", source_dir);
        return false;
    }

    qsort(list.items, list.count, sizeof(PackEntry), compare_pack_entries);

    printf("------\n");
    printf("Packing %zu Files (RGSS v%d)\n", list.count, version);
    printf("Output Archive: %s\n", out_archive);
    printf("------\n");

    FILE* out = fopen(out_archive, "wb");
    if (!out) {
        pack_list_free(&list);
        snprintf(err_msg, err_size, "Cannot create %.256s: %.128s", out_archive, strerror(errno));
        return false;
    }

    uint8_t header[8] = {'R', 'G', 'S', 'S', 'A', 'D', '\0', (uint8_t)version};
    if (fwrite(header, 1, sizeof(header), out) != sizeof(header)) {
        fclose(out);
        pack_list_free(&list);
        snprintf(err_msg, err_size, "Failed writing RGSS header");
        return false;
    }

    bool ok = (version == 1)
        ? write_pack_v1(out, &list, suppress, err_msg, err_size)
        : write_pack_v3(out, &list, suppress, err_msg, err_size);

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
