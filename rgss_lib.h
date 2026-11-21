#ifndef RGSS_LIB_H
#define RGSS_LIB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char* rgss_archive;
    char* output_dir;
    char* pack_source_dir;
    bool pack_mode;
    bool info_dump;
    bool suppress_output;
    bool create_project_file;
} ProgramArguments;

typedef struct {
    char* name;
    uint64_t offset;
    uint32_t size;
    uint32_t key;
} RgssFilePointer;

typedef struct {
    char* data_path;
    int version;
    RgssFilePointer* files;
    size_t file_count;
    size_t file_capacity;
} RgssArchive;

// Returns 1 for RGSSAD v1/v2, 3 for RGSSAD v3, -1 for invalid header, or 0 for unknown version.
int rgss_get_version(const char* path);

// Opens and parses the archive header and file table. Returns false on error and fills err_msg.
bool rgss_archive_open(RgssArchive* archive, const char* path, int version, char* err_msg, size_t err_size);

// Extracts and decrypts a single file entry to target_path. Enforces strict size check.
bool rgss_extract_file_to_disk(const RgssArchive* archive, const RgssFilePointer* fp,
                               const char* target_path, char* err_msg, size_t err_size);

// Packs a directory recursively into an RGSS1/2 (.rgssad/.rgss2a) or RGSS3 (.rgss3a) archive.
bool rgss_archive_pack(const char* source_dir, const char* out_archive_path, bool suppress,
                       char* err_msg, size_t err_size);

// Frees all heap memory associated with the archive.
void rgss_archive_free(RgssArchive* archive);

#ifdef __cplusplus
}
#endif

#endif // RGSS_LIB_H
