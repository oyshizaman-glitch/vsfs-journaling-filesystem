#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define BLOCK_SIZE       4096u
#define JOURNAL_START    1u
#define J_BLOCKS         16u
#define INODE_BMAP_BLOCK 17u
#define INODE_START_BLOCK 19u
#define ROOT_INODE_NUM   0u
#define J_MAGIC          0x4A524E4Cu
#define REC_DATA         1u
#define REC_COMMIT       2u

struct journal_hdr { uint32_t magic; uint32_t used_bytes; };
struct rec_hdr     { uint16_t type;  uint16_t size; };
struct inode {
    uint16_t type;
    uint16_t links;
    uint32_t size;
    uint32_t direct[8];
    uint32_t ctime;
    uint32_t mtime;
    uint8_t  _pad[128 - (2 + 2 + 4 + 8*4 + 4 + 4)];
};
_Static_assert(sizeof(struct inode) == 128, "inode must be 128 bytes");

struct dirent { uint32_t inode; char name[28]; };
_Static_assert(sizeof(struct dirent) == 32, "dirent must be 32 bytes");

static const uint32_t DATA_REC_SIZE   = (uint32_t)(sizeof(struct rec_hdr) + 4u + BLOCK_SIZE);
static const uint32_t COMMIT_REC_SIZE = (uint32_t)sizeof(struct rec_hdr);

static const int MAX_PENDING = 256;

// IO helper
static int must_read(int fd, void *buf, size_t n) {
    uint8_t *p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) return 0;
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        got += (size_t)r;
    }
    return 1;
}

static int must_write(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t*)buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        done += (size_t)w;
    }
    return 1;
}

static int seek_abs(int fd, uint32_t off) {
    return (lseek(fd, (off_t)off, SEEK_SET) != (off_t)-1);
}

static int read_block(int fd, uint32_t bno, void *buf) {
    if (!seek_abs(fd, bno * BLOCK_SIZE)) return 0;
    return must_read(fd, buf, BLOCK_SIZE);
}

static int write_block(int fd, uint32_t bno, const void *buf) {
    if (!seek_abs(fd, bno * BLOCK_SIZE)) return 0;
    return must_write(fd, buf, BLOCK_SIZE);
}

// Journal writer
static void write_data_rec(int fd, uint32_t *pos, uint32_t bno, const void *data) {
    struct rec_hdr rh;
    rh.type = REC_DATA;
    rh.size = (uint16_t)DATA_REC_SIZE;

    seek_abs(fd, *pos);
    must_write(fd, &rh, sizeof(rh));
    must_write(fd, &bno, 4);
    must_write(fd, data, BLOCK_SIZE);
    *pos += DATA_REC_SIZE;
}

static int journal_capacity_ok(uint32_t used_bytes, uint32_t txn_needed) {
    return (used_bytes + txn_needed) <= (J_BLOCKS * BLOCK_SIZE);
}

// *****CREATE command*****
static void do_create(const char *name) {
    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) { perror("open"); return; }
    if (!name || name[0] == '\0') {
        printf("Error: empty name\n");
        close(fd);
        return;
    }
    if (strlen(name) >= sizeof(((struct dirent*)0)->name)) {
        printf("Error: name too long (max 27 characters)\n");
        close(fd);
        return;
    }

    // read header
    struct journal_hdr j_hdr;
    if (!seek_abs(fd, JOURNAL_START * BLOCK_SIZE) || !must_read(fd, &j_hdr, sizeof(j_hdr))) {
        close(fd); return;
    }

    // init journal header if invalid
    if (j_hdr.magic != J_MAGIC ||
        j_hdr.used_bytes < sizeof(struct journal_hdr) ||
        j_hdr.used_bytes > (J_BLOCKS * BLOCK_SIZE)) {
        j_hdr.magic = J_MAGIC;
        j_hdr.used_bytes = sizeof(struct journal_hdr);
        seek_abs(fd, JOURNAL_START * BLOCK_SIZE);
        must_write(fd, &j_hdr, sizeof(j_hdr));
    }

    // home metadata blocks load
    uint8_t ibmap[BLOCK_SIZE], itable0[BLOCK_SIZE], itable1[BLOCK_SIZE];
    if (!read_block(fd, INODE_BMAP_BLOCK, ibmap) ||
        !read_block(fd, INODE_START_BLOCK, itable0) ||
        !read_block(fd, INODE_START_BLOCK + 1, itable1)) {
        close(fd); return;
    }

    // determine root dir block and read it
    uint32_t root_dir_bno = ((struct inode*)itable0)[ROOT_INODE_NUM].direct[0];

    uint8_t root_dir_content[BLOCK_SIZE];
    if (!read_block(fd, root_dir_bno, root_dir_content)) {
        close(fd); return;
    }

    // replay committed txns from journal into local buffers 
    uint32_t off = sizeof(struct journal_hdr);
    uint32_t pending_blks[MAX_PENDING];
    uint8_t *pending_data[MAX_PENDING];
    int count = 0;

    while (off + sizeof(struct rec_hdr) <= j_hdr.used_bytes) {
        struct rec_hdr rh;
        if (!seek_abs(fd, JOURNAL_START * BLOCK_SIZE + off) || !must_read(fd, &rh, sizeof(rh))) break;

        if (rh.size < sizeof(struct rec_hdr)) break;
        if (off + rh.size > j_hdr.used_bytes) break;

        if (rh.type == REC_DATA) {
            if (rh.size != DATA_REC_SIZE) break;
            if (count >= MAX_PENDING) break;

            uint32_t bno;
            if (!must_read(fd, &bno, 4)) break;

            pending_blks[count] = bno;
            pending_data[count] = (uint8_t*)malloc(BLOCK_SIZE);
            if (!pending_data[count]) break;

            if (!must_read(fd, pending_data[count], BLOCK_SIZE)) {
                free(pending_data[count]);
                break;
            }
            count++;
        } else if (rh.type == REC_COMMIT) {
            if (rh.size != COMMIT_REC_SIZE) break;

            for (int i = 0; i < count; i++) {
                if (pending_blks[i] == INODE_BMAP_BLOCK)
                    memcpy(ibmap, pending_data[i], BLOCK_SIZE);
                else if (pending_blks[i] == INODE_START_BLOCK)
                    memcpy(itable0, pending_data[i], BLOCK_SIZE);
                else if (pending_blks[i] == INODE_START_BLOCK + 1)
                    memcpy(itable1, pending_data[i], BLOCK_SIZE);
                else if (pending_blks[i] == root_dir_bno)
                    memcpy(root_dir_content, pending_data[i], BLOCK_SIZE);
                free(pending_data[i]);
            }
            count = 0;
        } else {
            break;
        }

        off += rh.size;
    }

    for (int i = 0; i < count; i++) free(pending_data[i]); // uncommitted tail drop

    // check exists plus find truly empty dir slot
    struct dirent *entries = (struct dirent*)root_dir_content;
    int slot = -1;

    for (int i = 0; i < (int)(BLOCK_SIZE / sizeof(struct dirent)); i++) {
        if (entries[i].inode == 0 && entries[i].name[0] == '\0') {
            if (slot == -1) slot = i;
        } else if (strcmp(entries[i].name, name) == 0) {
            printf("Error: File exists\n");
            close(fd);
            return;
        }
    }
    if (slot == -1) { printf("Error: Dir full\n"); close(fd); return; }

    // free inode finder
    int new_inum = -1;
    for (int i = 1; i < 64; i++) {
        if (((ibmap[i / 8] >> (i % 8)) & 1u) == 0u) { new_inum = i; break; }
    }
    if (new_inum == -1) { printf("Error: No inodes\n"); close(fd); return; }

    // journal space checker
    uint32_t data_recs = (new_inum >= 32) ? 4u : 3u;
    uint32_t txn_needed = data_recs * DATA_REC_SIZE + COMMIT_REC_SIZE;

    if (!journal_capacity_ok(j_hdr.used_bytes, txn_needed)) {
        printf("Error: Journal full. Run install.\n");
        close(fd);
        return;
    }

    // local buffers e apply updates
    ibmap[new_inum / 8] |= (1u << (new_inum % 8));

    struct inode file_node;
    memset(&file_node, 0, sizeof(file_node));
    file_node.type = 1;      // only files, folder not needed
    file_node.links = 1;
    file_node.size = 0;
    file_node.ctime = file_node.mtime = (uint32_t)time(NULL);

    if (new_inum < 32) ((struct inode*)itable0)[new_inum] = file_node;
    else              ((struct inode*)itable1)[new_inum - 32] = file_node;

    entries[slot].inode = (uint32_t)new_inum;
    strncpy(entries[slot].name, name, 27);
    entries[slot].name[27] = '\0';

    // grow root directory inode size
    struct inode *root_inode = &((struct inode*)itable0)[ROOT_INODE_NUM];
    uint32_t needed = (uint32_t)(slot + 1) * (uint32_t)sizeof(struct dirent);
    if (root_inode->size < needed) root_inode->size = needed;
    root_inode->mtime = (uint32_t)time(NULL);

    // append txn to journal
    uint32_t base = JOURNAL_START * BLOCK_SIZE;
    uint32_t curr = base + j_hdr.used_bytes;

    write_data_rec(fd, &curr, INODE_BMAP_BLOCK, ibmap);
    write_data_rec(fd, &curr, INODE_START_BLOCK, itable0);

    if (new_inum >= 32) {
        write_data_rec(fd, &curr, INODE_START_BLOCK + 1, itable1);
    }

    write_data_rec(fd, &curr, root_dir_bno, root_dir_content);

    struct rec_hdr commit;
    commit.type = REC_COMMIT;
    commit.size = (uint16_t)COMMIT_REC_SIZE;
    seek_abs(fd, curr);
    must_write(fd, &commit, sizeof(commit));
    curr += COMMIT_REC_SIZE;

    // update journal header used_bytes
    j_hdr.used_bytes = curr - base;
    seek_abs(fd, base);
    must_write(fd, &j_hdr, sizeof(j_hdr));

    printf("Created %s (inum %d) in journal\n", name, new_inum);
    close(fd);
}

// *****INSTALL command*****
static void do_install(void) {
    int fd = open("vsfs.img", O_RDWR);
    if (fd < 0) { perror("open"); return; }

    struct journal_hdr jh;
    if (!seek_abs(fd, JOURNAL_START * BLOCK_SIZE) || !must_read(fd, &jh, sizeof(jh))) {
        close(fd); return;
    }

    if (jh.magic != J_MAGIC ||
        jh.used_bytes <= sizeof(jh) ||
        jh.used_bytes > (J_BLOCKS * BLOCK_SIZE)) {
        close(fd);
        return;
    }

    uint32_t off = sizeof(jh);
    uint32_t pending_blks[MAX_PENDING];
    uint8_t *pending_data[MAX_PENDING];
    int count = 0;

    while (off + sizeof(struct rec_hdr) <= jh.used_bytes) {
        struct rec_hdr rh;
        if (!seek_abs(fd, JOURNAL_START * BLOCK_SIZE + off) || !must_read(fd, &rh, sizeof(rh))) break;

        if (rh.size < sizeof(struct rec_hdr)) break;
        if (off + rh.size > jh.used_bytes) break;

        if (rh.type == REC_DATA) {
            if (rh.size != DATA_REC_SIZE) break;
            if (count >= MAX_PENDING) break;

            uint32_t bno;
            if (!must_read(fd, &bno, 4)) break;

            pending_blks[count] = bno;
            pending_data[count] = (uint8_t*)malloc(BLOCK_SIZE);
            if (!pending_data[count]) break;

            if (!must_read(fd, pending_data[count], BLOCK_SIZE)) {
                free(pending_data[count]);
                break;
            }
            count++;
        } else if (rh.type == REC_COMMIT) {
            if (rh.size != COMMIT_REC_SIZE) break;

            for (int i = 0; i < count; i++) {
                write_block(fd, pending_blks[i], pending_data[i]);
                free(pending_data[i]);
            }
            count = 0;
        } else {
            break;
        }

        off += rh.size;
    }

    for (int i = 0; i < count; i++) free(pending_data[i]); // uncommitted tail drop


    jh.used_bytes = sizeof(jh);
    seek_abs(fd, JOURNAL_START * BLOCK_SIZE);
    must_write(fd, &jh, sizeof(jh));

    printf("Journal installed.\n");
    close(fd);
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "create") == 0) {
        do_create(argv[2]);
    } else if (argc == 2 && strcmp(argv[1], "install") == 0) {
        do_install();
    } else {
        printf("Usage: %s create <name> | %s install\n", argv[0], argv[0]);
        return 1;
    }
    return 0;
}
