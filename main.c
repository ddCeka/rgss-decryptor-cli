#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
    #include <direct.h>
    #include <io.h>
    #define MKDIR(dir) _mkdir(dir)
    #define PATH_SEP '\\'
    #define FILE_EXISTS(path) (_access((path), 0) == 0)
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <unistd.h>
    #define MKDIR(dir) mkdir((dir), 0755)
    #define PATH_SEP '/'
    #define FILE_EXISTS(path) (access((path), F_OK) == 0)
#endif

#include "rgss_lib.h"

#define MAX_PATH_LEN 4096

static bool join_paths(char* dst, size_t dst_size, const char* a, const char* b) {
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

static void str_right(const char* src, size_t len, char* dest, size_t dest_size) {
    if (!src || dest_size == 0) {
        if (dest_size > 0) dest[0] = '\0';
        return;
    }
    size_t src_len = strlen(src);
    const char* start = (src_len > len) ? (src + (src_len - len)) : src;
    snprintf(dest, dest_size, "%s", start);
}

static bool is_safe_rel_path(const char* rel_path) {
    if (!rel_path || rel_path[0] == '\0') return false;
    if (rel_path[0] == '/' || rel_path[0] == '\\') return false;
    if (strlen(rel_path) >= 2 && isalpha((unsigned char)rel_path[0]) && rel_path[1] == ':') {
        return false;
    }

    const char* seg_start = rel_path;
    for (const char* p = rel_path; ; p++) {
        if (*p == '/' || *p == '\\' || *p == '\0') {
            size_t seg_len = (size_t)(p - seg_start);
            if (seg_len == 2 && seg_start[0] == '.' && seg_start[1] == '.') {
                return false;
            }
            if (*p == '\0') break;
            seg_start = p + 1;
        }
    }
    return true;
}

static void mkdir_recursive(const char* file_path) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", file_path);

    char* last_slash = NULL;
    for (char* p = tmp; *p; p++) {
        if (*p == '\\' || *p == '/') {
            *p = PATH_SEP;
            last_slash = p;
        }
    }
    if (!last_slash) return;
    *last_slash = '\0';

    for (char* p = tmp + 1; *p; p++) {
        if (*p == PATH_SEP) {
            *p = '\0';
            MKDIR(tmp);
            *p = PATH_SEP;
        }
    }
    MKDIR(tmp);
}

static bool build_output_dir(const char* archive_path, const char* output_arg,
                             char* out_dir, size_t out_size) {
    if (output_arg && (output_arg[0] == '/' || output_arg[0] == '\\' ||
        (strlen(output_arg) > 1 && output_arg[1] == ':'))) {
        if (strlen(output_arg) >= out_size) return false;
        snprintf(out_dir, out_size, "%s", output_arg);
        return true;
    }

    char base_dir[MAX_PATH_LEN];
    snprintf(base_dir, sizeof(base_dir), "%s", archive_path);

    char* last_sep = strrchr(base_dir, '\\');
    char* last_fsep = strrchr(base_dir, '/');
    if (last_fsep && (!last_sep || last_fsep > last_sep)) {
        last_sep = last_fsep;
    }

    if (last_sep) {
        *last_sep = '\0';
    } else {
        snprintf(base_dir, sizeof(base_dir), ".");
    }

    if (!output_arg || output_arg[0] == '\0' || strcmp(output_arg, ".") == 0) {
        if (strlen(base_dir) >= out_size) return false;
        snprintf(out_dir, out_size, "%s", base_dir);
        return true;
    }

    return join_paths(out_dir, out_size, base_dir, output_arg);
}

static void create_project_file(const char* rgss_data_file, const char* out_dir) {
    const char* dot = strrchr(rgss_data_file, '.');
    if (!dot) {
        printf("%sCouldn't Determine RPG Maker Version%s\n", g_colors.err, g_colors.reset);
        return;
    }

    char ext[16] = {0};
    for (size_t i = 0; dot[i] && i < sizeof(ext) - 1; i++) {
        ext[i] = (char)tolower((unsigned char)dot[i]);
    }

    const char* proj_name = NULL;
    const char* proj_content = NULL;

    if (strcmp(ext, ".rgssad") == 0) {
        proj_name = "Game.rxproj";
        proj_content = "RPGXP 1.02";
    } else if (strcmp(ext, ".rgss2a") == 0) {
        proj_name = "Game.rvproj";
        proj_content = "RPGVX 1.02";
    } else if (strcmp(ext, ".rgss3a") == 0) {
        proj_name = "Game.rvproj2";
        proj_content = "RPGVXAce 1.00";
    } else {
        printf("%sCouldn't Determine RPG Maker Version%s\n", g_colors.err, g_colors.reset);
        return;
    }

    char full_path[MAX_PATH_LEN];
    if (!join_paths(full_path, sizeof(full_path), out_dir, proj_name)) {
        fprintf(stderr, "%sProject file path too long%s\n", g_colors.err_stderr, g_colors.reset_stderr);
        return;
    }

    FILE* f = fopen(full_path, "wb");
    if (f) {
        fwrite(proj_content, 1, strlen(proj_content), f);
        fclose(f);
    }
}

static void dump_info(const RgssArchive* archive) {
    printf("%s%-6s%s|%s%-8s%s|%s%-42s%s|%s%-10s%s|%s%-8s%s\n",
           g_colors.bold, "Num", g_colors.muted,
           g_colors.bold, "Offset", g_colors.muted,
           g_colors.bold, "Name", g_colors.muted,
           g_colors.bold, "Size Bytes", g_colors.muted,
           g_colors.bold, "Key", g_colors.reset);
    printf("%s------|--------|------------------------------------------|----------|--------%s\n",
           g_colors.muted, g_colors.reset);

    for (size_t i = 0; i < archive->file_count; i++) {
        const RgssFilePointer* ptr = &archive->files[i];
        char clip_name[64];
        char name_highlight[128];

        str_right(ptr->name, 42, clip_name, sizeof(clip_name));
        rgss_format_highlighted_name(clip_name, 42, name_highlight, sizeof(name_highlight));

        printf("%s%6zu%s|%s%08llX%s|%s%s|%s%-10u%s|%s%08X%s\n",
               g_colors.muted, i, g_colors.muted,
               g_colors.accent, (unsigned long long)ptr->offset, g_colors.muted,
               name_highlight, g_colors.muted,
               g_colors.warn, ptr->size, g_colors.muted,
               g_colors.ok, ptr->key, g_colors.reset);
    }

    printf("%s------|--------|------------------------------------------|----------|--------%s\n",
           g_colors.muted, g_colors.reset);
}

static bool extract_files(const RgssArchive* archive, const char* out_dir, bool suppress,
                          char* err_msg, size_t err_size) {
    size_t file_count = archive->file_count;
    int digits = (file_count > 0) ? (int)floor(log10((double)file_count) + 1) : 1;

    printf("%s------%s\n", g_colors.muted, g_colors.reset);
    printf("%s%zu%s Files Found\n", g_colors.bold, file_count, g_colors.reset);
    printf("Output Folder: %s%s%s\n", g_colors.dir, out_dir, g_colors.reset);
    printf("%s------%s\n", g_colors.muted, g_colors.reset);

    for (size_t i = 0; i < file_count; i++) {
        const RgssFilePointer* ptr = &archive->files[i];

        if (!is_safe_rel_path(ptr->name)) {
            snprintf(err_msg, err_size, "Unsafe path traversal detected in entry: %.256s", ptr->name);
            return false;
        }

        char rel_path[MAX_PATH_LEN];
        snprintf(rel_path, sizeof(rel_path), "%s", ptr->name);
        for (char* p = rel_path; *p; p++) {
            if (*p == '\\' || *p == '/') *p = PATH_SEP;
        }

        char target_path[MAX_PATH_LEN];
        if (!join_paths(target_path, sizeof(target_path), out_dir, rel_path)) {
            snprintf(err_msg, err_size, "Output path too long for entry: %.256s", ptr->name);
            return false;
        }

        if (!suppress) {
            bool exists = FILE_EXISTS(target_path);
            char name_highlight[MAX_PATH_LEN + 64];
            rgss_format_highlighted_name(ptr->name, 0, name_highlight, sizeof(name_highlight));

            if (exists) {
                printf("%s[%*zu/%zu] Overwrite:%s %s\n",
                       g_colors.warn, digits, i + 1, file_count, g_colors.reset, name_highlight);
            } else {
                printf("%s[%*zu/%zu] Create   :%s %s\n",
                       g_colors.ok, digits, i + 1, file_count, g_colors.reset, name_highlight);
            }
        }

        mkdir_recursive(target_path);

        if (!rgss_extract_file_to_disk(archive, ptr, target_path, err_msg, err_size)) {
            return false;
        }
    }

    return true;
}

static void print_usage(const char* prog_name) {
    printf("%sUsage:%s\n", g_colors.bold, g_colors.reset);
    printf("  %s%s%s [%soptions%s] %s<archive.rgssad|rgss2a|rgss3a>%s\n",
           g_colors.file, prog_name, g_colors.reset,
           g_colors.ok, g_colors.reset,
           g_colors.accent, g_colors.reset);
    printf("  %s%s%s %s--pack%s %s<source_dir> <output.rgssad|rgss2a|rgss3a>%s\n\n",
           g_colors.file, prog_name, g_colors.reset,
           g_colors.ok, g_colors.reset,
           g_colors.accent, g_colors.reset);

    printf("%sUnpack Options:%s\n", g_colors.bold, g_colors.reset);
    printf("  %s-o, --output%s %s<dir>%s    Output directory relative to archive\n",
           g_colors.ok, g_colors.reset, g_colors.accent, g_colors.reset);
    printf("                        %s(default: archive dir)%s\n",
           g_colors.muted, g_colors.reset);
    printf("  %s-i, --info%s            Dump archive file table without extracting\n",
           g_colors.ok, g_colors.reset);
    printf("  %s-s, --suppress%s        Suppress per-file output\n",
           g_colors.ok, g_colors.reset);
    printf("  %s-p, --project%s         Generate RPG Maker project file\n",
           g_colors.ok, g_colors.reset);
    printf("                        %s(Game.rxproj/rvproj/rvproj2)%s\n\n",
           g_colors.muted, g_colors.reset);

    printf("%sRepack Options:%s\n", g_colors.bold, g_colors.reset);
    printf("  %s--pack%s   %s<dir>%s        Pack directory into specified RGSS archive\n",
           g_colors.ok, g_colors.reset, g_colors.accent, g_colors.reset);
    printf("  %s--repack%s %s<dir>%s        An alias of %s--pack%s command option\n\n",
           g_colors.ok, g_colors.reset, g_colors.accent, g_colors.reset,
           g_colors.ok, g_colors.reset);

    printf("%sGeneral:%s\n", g_colors.bold, g_colors.reset);
    printf("  %s-h, --help%s            Show this help message\n",
           g_colors.ok, g_colors.reset);
}

int main(int argc, char* argv[]) {
    rgss_init_colors();

    ProgramArguments args = {
        .rgss_archive = NULL,
        .output_dir = NULL,
        .pack_source_dir = NULL,
        .pack_mode = false,
        .info_dump = false,
        .suppress_output = false,
        .create_project_file = false
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-i") == 0 || strcmp(argv[i], "--info") == 0) {
            args.info_dump = true;
        } else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--suppress") == 0) {
            args.suppress_output = true;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--project") == 0) {
            args.create_project_file = true;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            args.output_dir = argv[++i];
        } else if ((strcmp(argv[i], "--pack") == 0 || strcmp(argv[i], "--repack") == 0) && i + 1 < argc) {
            args.pack_mode = true;
            args.pack_source_dir = argv[++i];
        } else if (argv[i][0] != '-') {
            args.rgss_archive = argv[i];
        } else {
            fprintf(stderr, "%sUnknown or incomplete option: %s%s\n",
                    g_colors.err_stderr, argv[i], g_colors.reset_stderr);
            print_usage(argv[0]);
            return 1;
        }
    }

    char err_msg[512] = {0};

    if (args.pack_mode) {
        if (args.info_dump || args.create_project_file || args.output_dir != NULL) {
            fprintf(stderr, "%sError: --pack/--repack cannot be combined with -i, -p, or -o.%s\n",
                    g_colors.err_stderr, g_colors.reset_stderr);
            return 1;
        }
        if (!args.pack_source_dir || !args.rgss_archive) {
            fprintf(stderr, "%sError: --pack/--repack requires both <source_dir> and <output_archive>.%s\n",
                    g_colors.err_stderr, g_colors.reset_stderr);
            print_usage(argv[0]);
            return 1;
        }
        if (!rgss_archive_pack(args.pack_source_dir, args.rgss_archive, args.suppress_output,
                               err_msg, sizeof(err_msg))) {
            fprintf(stderr, "%sError packing archive: %s%s\n",
                    g_colors.err_stderr, err_msg, g_colors.reset_stderr);
            return 1;
        }
        return 0;
    }

    if (!args.rgss_archive || args.rgss_archive[0] == '\0') {
        print_usage(argv[0]);
        return 1;
    }

    int ver = rgss_get_version(args.rgss_archive);
    if (ver == -1) {
        printf("%sInvalid RGSSAD Archive%s\n", g_colors.err, g_colors.reset);
        return 1;
    } else if (ver != 1 && ver != 3) {
        printf("%sUnknown RGSSAD Version%s\n", g_colors.err, g_colors.reset);
        return 1;
    }

    RgssArchive archive = {0};
    if (!rgss_archive_open(&archive, args.rgss_archive, ver, err_msg, sizeof(err_msg))) {
        printf("%sError reading the archive: %s%s\n", g_colors.err, err_msg, g_colors.reset);
        return 1;
    }

    char out_dir[MAX_PATH_LEN];
    if (!build_output_dir(args.rgss_archive, args.output_dir, out_dir, sizeof(out_dir))) {
        printf("%sError: Output directory path is too long%s\n", g_colors.err, g_colors.reset);
        rgss_archive_free(&archive);
        return 1;
    }

    int exit_code = 0;
    if (args.info_dump) {
        dump_info(&archive);
    } else {
        if (!extract_files(&archive, out_dir, args.suppress_output, err_msg, sizeof(err_msg))) {
            printf("%sError extracting files: %s%s\n", g_colors.err, err_msg, g_colors.reset);
            exit_code = 1;
        } else if (args.create_project_file) {
            create_project_file(args.rgss_archive, out_dir);
        }
    }

    rgss_archive_free(&archive);
    return exit_code;
}
