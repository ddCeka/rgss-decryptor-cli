#include "rgss_lib.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RGSS_MAGIC "RGSSAD\0"
#define RGSS_MAGIC_LEN 7
#define RGSS_V1_INITIAL_KEY 0xDEADCAFEu
#define IO_BUFFER_SIZE (16 * 1024)

static bool read_u32_le(FILE* f, uint32_t* out) {
    uint8_t b[4];
    if (fread(b, 1, 4, f) != 4) return false;
    *out = (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
    return true;
}

static bool read_u8(FILE* f, uint8_t* out) {
    return fread(out, 1, 1, f) == 1;
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
            free(name);
            snprintf(err_msg, err_size, "Unexpected EOF reading entry size for %s", name);
            return false;
        }
        data_size ^= step_key(&key);

        uint64_t data_offset = tell_u64(f);
        if (data_offset + data_size > file_size) {
            snprintf(err_msg, err_size, "Entry %s exceeds archive bounds", name);
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
            snprintf(err_msg, err_size, "Failed to seek past entry %s", name);
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
            snprintf(err_msg, err_size, "Entry %s exceeds archive bounds", name);
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

bool rgss_extract_file_to_disk(const RgssArchive* archive, const RgssFilePointer* fp,
                               const char* target_path, char* err_msg, size_t err_size) {
    FILE* in = fopen(archive->data_path, "rb");
    if (!in) {
        snprintf(err_msg, err_size, "Failed to open archive: %s", strerror(errno));
        return false;
    }

    if (!seek_u64(in, fp->offset, SEEK_SET)) {
        fclose(in);
        snprintf(err_msg, err_size, "Failed to seek to %s", fp->name);
        return false;
    }

    FILE* out = fopen(target_path, "wb");
    if (!out) {
        fclose(in);
        snprintf(err_msg, err_size, "Failed to create %s: %s", target_path, strerror(errno));
        return false;
    }

    uint8_t buffer[IO_BUFFER_SIZE];
    uint32_t key = fp->key;
    uint32_t current_mask = step_key(&key);
    int byte_lane = 0;
    size_t total_read = 0;

    while (total_read < fp->size) {
        size_t remaining = (size_t)fp->size - total_read;
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
            fclose(in);
            fclose(out);
            snprintf(err_msg, err_size, "Write error on %s: %s", target_path, strerror(errno));
            return false;
        }

        total_read += n;
    }

    fclose(in);
    fclose(out);

    if (total_read < fp->size) {
        snprintf(err_msg, err_size, "Incomplete file %s (%zu/%u bytes)",
                 fp->name, total_read, fp->size);
        return false;
    }

    return true;
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
