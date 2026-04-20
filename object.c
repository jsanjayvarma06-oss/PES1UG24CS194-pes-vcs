// object.c — Content-addressable object store
#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/sha.h>

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++)
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, len);
    SHA256_Final(id_out->hash, &ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    const char *type_str;
    if (type == OBJ_BLOB)        type_str = "blob";
    else if (type == OBJ_TREE)   type_str = "tree";
    else if (type == OBJ_COMMIT) type_str = "commit";
    else return -1;

    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;

    size_t total = header_len + len;
    uint8_t *full = malloc(total);
    if (!full) return -1;
    memcpy(full, header, header_len);
    memcpy(full + header_len, data, len);

    compute_hash(full, total, id_out);

    if (object_exists(id_out)) { free(full); return 0; }

    char path[512], dir[512], tmp_path[520];
    object_path(id_out, path, sizeof(path));
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id_out, hex);
    snprintf(dir, sizeof(dir), "%s/%.2s", OBJECTS_DIR, hex);
    mkdir(dir, 0755);

    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }
    ssize_t written = write(fd, full, total);
    free(full);
    if (written < 0) { close(fd); return -1; }
    fsync(fd);
    close(fd);
    if (rename(tmp_path, path) != 0) return -1;

    int dir_fd = open(dir, O_RDONLY);
    if (dir_fd >= 0) { fsync(dir_fd); close(dir_fd); }
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    size_t total = ftell(f);
    rewind(f);
    uint8_t *buf = malloc(total);
    if (!buf) { fclose(f); return -1; }
    size_t rd = fread(buf, 1, total, f);
    fclose(f);
    if (rd != total) { free(buf); return -1; }

    ObjectID computed;
    compute_hash(buf, total, &computed);
    if (memcmp(computed.hash, id->hash, HASH_SIZE) != 0) { free(buf); return -1; }

    uint8_t *null_pos = memchr(buf, '\0', total);
    if (!null_pos) { free(buf); return -1; }

    if (strncmp((char*)buf, "blob ", 5) == 0)        *type_out = OBJ_BLOB;
    else if (strncmp((char*)buf, "tree ", 5) == 0)   *type_out = OBJ_TREE;
    else if (strncmp((char*)buf, "commit ", 7) == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    uint8_t *data_start = null_pos + 1;
    *len_out = total - (data_start - buf);
    *data_out = malloc(*len_out + 1);
    if (!*data_out) { free(buf); return -1; }
    memcpy(*data_out, data_start, *len_out);
    ((char *)*data_out)[*len_out] = '\0';
    free(buf);
    return 0;
}
