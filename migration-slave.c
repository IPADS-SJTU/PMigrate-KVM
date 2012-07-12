#include <signal.h>

#include "qemu-common.h"
#include "qemu_socket.h"
#include "migration.h"
#include "qemu-char.h"
#include "sysemu.h"
#include "buffered_file.h"
#include "block.h"
#include "quicklz.h"
#include "zlib.h"

#define MULTI_TRY 100

#define DEBUG_MIGRATION_SLAVE

#ifdef DEBUG_MIGRATION_SLAVE
#define DPRINTF(fmt, ...) \
    do { printf("migration_slave: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

//#define ENABLE_QUICKLZ 1
#define ENABLE_ZLIB 1

typedef unsigned char Byte;
typedef unsigned char Bytef;

__thread Bytef *decomp_buf;
__thread Bytef *decomped_buf;
__thread Bytef *decomped_ptr;
__thread Bytef *decomp_ptr;
__thread Bytef *comp_buf;
__thread Bytef *comped_buf;
__thread Bytef *comp_ptr;
__thread long mcomp_time;
__thread long mcomp_size;
__thread long mcomped_size;
__thread long dcomp_time;
__thread long dcomp_size;
__thread long dcomped_size;

__thread qlz_state_compress *state_compress;

FdMigrationStateSlave *
tcp_start_outgoing_migration_slave(Monitor *mon,
                                   char *dest_ip,
                                   int64_t bandwidth_limit,
                                   int SSL_type, int compression);
void * start_host_slave(void *data);
void init_host_slaves(struct FdMigrationState *s);
void *start_dest_slave(void *data);



static int socket_errno_slave(FdMigrationStateSlave *s) {
    return errno;
}

/*
 * classicsong add ssl op here
 */
static int socket_write_ssl(FdMigrationStateSlave *s, const void * buf, size_t size)
{
    return send(s->fd, buf, size, 0);
}

static int socket_write_slave(FdMigrationStateSlave *s, const void * buf, size_t size)
{
    return send(s->fd, buf, size, 0);
}

static int tcp_close_slave(FdMigrationStateSlave *s)
{
    DPRINTF("tcp_close\n");
    if (s->fd != -1) {
        close(s->fd);
        s->fd = -1;
    }
    return 0;
}

//borrowed from savevm.c
#define QEMU_VM_EOF                  0x00
#define QEMU_VM_SECTION_START        0x01
#define QEMU_VM_SECTION_PART         0x02
#define QEMU_VM_SECTION_END          0x03
#define QEMU_VM_SECTION_FULL         0x04
#define QEMU_VM_SUBSECTION           0x05
#define QEMU_VM_ITER_END             0x07
//borrowed from block-migration.c
#define BLK_MIG_FLAG_EOS                0x02
#define COMPRESS_LEVEL          5 //Z_DEFAULT_COMPRESSION
//borrowed from arch_init.c
#define RAM_SAVE_FLAG_EOS      0x10
#define COMPRESS_BUFSIZE        (12 << 20) /* Compression buffer  12MB */
#define COMPRESS_IGNORE         (32 << 10)

/*
 * host_port: the target connection ip:port
 */
FdMigrationStateSlave *
tcp_start_outgoing_migration_slave(Monitor *mon,
                                   char *dest_ip,
                                   int64_t bandwidth_limit,
                                   int SSL_type, int compression) {
    FdMigrationStateSlave *s;

    DPRINTF("Creating migration slave structure\n");
    s = qemu_mallocz(sizeof(*s));

    s->get_error = socket_errno_slave;//report socket error
    
    /*
     * register SSL handler
     */
    if (SSL_type == SSL_NO)
        s->write = socket_write_slave;//write data to the target fd
    else if (SSL_type == SSL_STRONG)
        s->write = socket_write_ssl;
    else {
        fprintf(stderr, "wrong ssl type use defult ssl solution");
        s->write = socket_write_slave;
    }
    s->compression = compression;
    s->close = tcp_close_slave;//close tcp connection
    s->mig_state.cancel = migrate_fd_cancel;//migartion cancel callback func
    s->mig_state.get_status = migrate_fd_get_status;//get current migration status
    s->mig_state.release = migrate_fd_release;//release migration fd | callback func for the end of migration

    s->state = MIG_STATE_ACTIVE;
    s->mon = mon;
    s->bandwidth_limit = bandwidth_limit;
    s->dest_ip = dest_ip;

    return s;
}

extern unsigned long disk_save_block_slave(void *ptr, int iter_num, QEMUFile *f);
extern unsigned long ram_save_block_slave(unsigned offset, uint8_t *p, void *block_p,
                                 struct FdMigrationStateSlave *s, int mem_vnum);
void *
start_host_slave(void *data) {
    FdMigrationStateSlave *s = (FdMigrationStateSlave *)data;
    struct task_body *body;
    struct sockaddr_in addr;
    int i, ret;
    QEMUFile *f;
    struct timespec slave_sleep = {0, 1000000};
    unsigned long data_sent;
    long comp_pos, len, comped_len;
     struct timeval start_tv, end_tv;   
    if (parse_host_port(&addr, s->dest_ip) < 0) {
        fprintf(stderr, "wrong dest ip %s\n", s->dest_ip);
        return NULL;
    }

    DPRINTF("Start host slave, begin creating connection, %s, %ld\n", s->dest_ip, s->bandwidth_limit);
    /*
     * create network connection
     */
    s->fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (s->fd == -1) {
        fprintf(stderr, "error creating socket\n");
        qemu_free(s);
        return NULL;
    }
    

    if (s->compression){
       comp_buf = (Bytef *)malloc(COMPRESS_BUFSIZE);
       comped_buf = (Bytef *)malloc(COMPRESS_BUFSIZE);
       comp_ptr = comp_buf;
       mcomp_time = 0;
       mcomp_size = 0;
       mcomped_size = 0;
       dcomp_time = 0;
       dcomp_size = 0;
       dcomped_size = 0;
    }

    //socket_set_nonblock(s->fd);
    
    for (i = 0; i < MULTI_TRY; i++) {
        if (connect(s->fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {

            ret = s->get_error(s);
            if (ret == EINVAL) {
                fprintf(stderr, "slave network connection param error %s:%d\n", s->dest_ip, ret);
                return NULL;
            }

            nanosleep(&slave_sleep, NULL);
            continue;
        }

        break;
    }

    DPRINTF("Connection build %s\n", s->dest_ip);

    /*
     * create file ops
     */
    s->file = qemu_fopen_ops_buffered_slave(s,
                                            s->bandwidth_limit,
                                            migrate_fd_put_buffer_slave,
                                            migrate_fd_put_ready_slave,
                                            migrate_fd_wait_for_unfreeze,
                                            migrate_fd_close);

    f = s->file;
    pthread_barrier_wait(&s->sender_barr->sender_iter_barr);

    DPRINTF("slave start migration, %lx, file %p\n", s->bandwidth_limit/1024, f);
    /*
     * wait for following commands
     * As disk task maybe limited by the disk throughput, so we perfer to transfer disk first and then memory
     * While sending data in one iteration, we assume the total throughput of this iteration is static
     * So the effect of sending memory first and sending disk first is same.
     */
    while (1) {
        void *body_p;
        /* check for disk */
        if (queue_pop_task(s->disk_task_queue, &body_p) > 0) {
            body = (struct task_body *)body_p;
           // DPRINTF("get disk task, %d, section id %d\n", s->mem_task_queue->iter_num,
           //         s->mem_task_queue->section_id);

            /* Section type */
            qemu_put_byte(f, QEMU_VM_SECTION_PART);
            qemu_put_be32(f, s->disk_task_queue->section_id);

            if (s->compression){
//                DPRINTF("BLOCK chunk\n");
                for (i = 0; i < body->len; i++) { 
                    s->disk_task_queue->slave_sent[s->id] += disk_putbuf_block_slave(body->blocks[i].ptr,
                                                  body->iter_num);
                }
                buf_put_be64(BLK_MIG_FLAG_EOS);
                comped_len = COMPRESS_BUFSIZE;
                gettimeofday(&start_tv, NULL);

#ifdef ENABLE_QUICKLZ
  	        state_compress = (qlz_state_compress *)malloc(sizeof(qlz_state_compress));
	        comped_len = qlz_compress(comp_ptr,comped_buf,  &comp_buf[0] -&comp_ptr[0], state_compress);
	        free(state_compress);
#endif
#ifdef ENABLE_ZLIB
               if (&comp_buf[0] - &comp_ptr[0] > COMPRESS_IGNORE)
                    compress2(comped_buf, &comped_len, comp_ptr, &comp_buf[0] - &comp_ptr[0], COMPRESS_LEVEL);
                else
                    compress2(comped_buf, &comped_len, comp_ptr, &comp_buf[0] - &comp_ptr[0], 0);
#endif
                gettimeofday(&end_tv, NULL);
                dcomp_time += (end_tv.tv_sec - start_tv.tv_sec) * 1000 + (end_tv.tv_usec - start_tv.tv_usec) / 1000;
                dcomp_size += &comp_buf[0] - &comp_ptr[0];
                dcomped_size += comped_len;
//                DPRINTF("disk compressed: %d -> %d\n", &comp_buf[0] -&comp_ptr[0],  comped_len);
                qemu_put_be32(f, comped_len);
#ifdef ENABLE_QUICKLZ
		qemu_put_be32(f,  &comp_buf[0] - &comp_ptr[0]);
#endif
                qemu_put_buffer(f, comped_buf, comped_len); 
                qemu_fflush(f);
/*                DPRINTF("PACKED HEAD:\n");
                int j;
                for (j = 0; j < 100; j++)
                    printf("%x|", comp_buf[j]);
                printf("\n"); */
                comp_buf = comp_ptr;
                free(body);
            }else{          
                /*
                 * handle disk
                 */
                for (i = 0; i < body->len; i++) {
                    s->disk_task_queue->slave_sent[s->id] += 
                        disk_save_block_slave(body->blocks[i].ptr, 
                                              body->iter_num, s->file);
                }
                /* End of the single task */
                qemu_put_be64(f, BLK_MIG_FLAG_EOS);
                qemu_fflush(f);

                free(body);
            }

       }
        /* check for memory */
        else if (queue_pop_task(s->mem_task_queue, &body_p) > 0) {
            body = (struct task_body *)body_p;
            //DPRINTF("get mem task, %lx: %p, %d, section id %d\n", body->pages[0].addr, 
            //     body->pages[0].ptr, 
            //      s->mem_task_queue->iter_num, s->mem_task_queue->section_id);
            /* Section type */
            
            qemu_put_byte(f, QEMU_VM_SECTION_PART);
            qemu_put_be32(f, s->mem_task_queue->section_id);
            if(s->compression){
                for (i = 0; i < body->len; i++) {
                    s->mem_task_queue->slave_sent[s->id] += ram_putbuf_block_slave(body->pages[i].addr, body->pages[i].ptr, 
                                             body->pages[i].block, s->mem_task_queue->iter_num);
                }                
                buf_put_be64(RAM_SAVE_FLAG_EOS);
                comped_len = COMPRESS_BUFSIZE;
                gettimeofday(&start_tv, NULL);
#ifdef ENABLE_QUICKLZ
  	        state_compress = (qlz_state_compress *)malloc(sizeof(qlz_state_compress));
       comped_len = qlz_compress(comp_ptr,comped_buf,  &comp_buf[0] -&comp_ptr[0], state_compress);
       free(state_compress);
#endif
#ifdef ENABLE_ZLIB
                if (&comp_buf[0] - &comp_ptr[0] > COMPRESS_IGNORE)
                    compress2(comped_buf, &comped_len, comp_ptr, &comp_buf[0] - &comp_ptr[0], COMPRESS_LEVEL);
                else
                    compress2(comped_buf, &comped_len, comp_ptr, &comp_buf[0] - &comp_ptr[0], 0);
#endif
                gettimeofday(&end_tv, NULL);
                mcomp_time += (end_tv.tv_sec - start_tv.tv_sec) * 1000 + (end_tv.tv_usec - start_tv.tv_usec) / 1000;
                mcomp_size += &comp_buf[0] - &comp_ptr[0];
                mcomped_size += comped_len;
//                DPRINTF("mem compressed: %d -> %d\n", &comp_buf[0] -&comp_ptr[0],  comped_len);
                qemu_put_be32(f, comped_len);
#ifdef ENABLE_QUICKLZ
		qemu_put_be32(f,  &comp_buf[0] - &comp_ptr[0]);
#endif
                qemu_put_buffer(f, comped_buf, comped_len);
                qemu_fflush(f);
/*                DPRINTF("PACKED HEAD:\n");
                int j;
                for (j = 0; j < 100; j++)
                    printf("%x|", comp_buf[j]);
                printf("\n");               */
                comp_buf = &comp_ptr[0];
                free(body);
            }else{
                for (i = 0; i < body->len; i++) {
                    s->mem_task_queue->slave_sent[s->id] += 
                        ram_save_block_slave(body->pages[i].addr, body->pages[i].ptr, 
                                             body->pages[i].block, s, s->mem_task_queue->iter_num);
                }            
                /* End of the single task */
                qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
                qemu_fflush(f);
                free(body);
            }
        }
        /* no disk and memory task */
        else {
            if (s->sender_barr->mem_state == BARR_STATE_ITER_END && 
                s->sender_barr->disk_state == BARR_STATE_ITER_END) {
                char buf[4];
                int rret;
                DPRINTF("Iteration End fall into barriers\n");
                qemu_put_byte(f, QEMU_VM_ITER_END);
                qemu_fflush(f);
                rret = read(s->fd, buf, sizeof("OK"));
                pthread_barrier_wait(&s->sender_barr->sender_iter_barr);
                pthread_barrier_wait(&s->sender_barr->next_iter_barr);
            }
            else if (s->sender_barr->mem_state == BARR_STATE_ITER_TERMINATE &&
                     s->sender_barr->disk_state == BARR_STATE_ITER_TERMINATE) {
                DPRINTF("Last Iteration End\n");
                qemu_put_byte(f, QEMU_VM_EOF);
                qemu_fflush(f);
                pthread_barrier_wait(&s->sender_barr->sender_iter_barr);

                data_sent = 0;
                break;
            }

            //get nothing, wait for a moment
            nanosleep(&slave_sleep, NULL);
        }
    }
    if (s->compression){
        free(comp_buf);
        free(comped_buf);
        DPRINTF("slave[%d] [M/D]comp_time=%ld/%ld, ratio=%ld/%ld->%ld/%ld[%f/%f]\n",s->id, mcomp_time, dcomp_time, mcomped_size, dcomped_size, mcomp_size, dcomp_size, mcomped_size / (mcomp_size + 0.0),  dcomped_size / (dcomp_size + 0.0));
    }
        
    DPRINTF("slave terminate\n");
    return NULL;
}

void init_host_slaves(struct FdMigrationState *s) {
    struct ip_list *next_ip;
    int i;

    DPRINTF("Start init slaves %d\n", s->para_config->num_slaves);
    s->sender_barr = (struct migration_barrier *)malloc(sizeof(struct migration_barrier));
    init_migr_barrier(s->sender_barr, s->para_config->num_slaves);
    
    next_ip = s->para_config->dest_ip_list;
    for (i = 0; i < s->para_config->num_slaves; i ++) {
        FdMigrationStateSlave *slave_s;
        pthread_t tid;
        struct migration_slave *slave = (struct migration_slave *)malloc(sizeof(struct migration_slave));
        char *dest_ip = next_ip->host_port;
        int ssl_type = s->para_config->SSL_type;
        int compression = s->para_config->compression;
        slave_s = tcp_start_outgoing_migration_slave(s->mon, 
                                                     dest_ip, s->para_config->default_throughput,
                                                     ssl_type, compression);
        slave_s->mem_task_queue = s->mem_task_queue;
        slave_s->disk_task_queue = s->disk_task_queue;
        slave_s->sender_barr = s->sender_barr;
        slave_s->id = i;

        DPRINTF("slave_s is %p\n", slave_s);
        pthread_create(&tid, NULL, start_host_slave, slave_s);
        slave->slave_id = tid;
        slave->next = s->slave_list;
        s->slave_list = slave;

        next_ip = next_ip->next;
    }
}

struct dest_slave_para{
    char *listen_ip;
    int ssl_type;
    int compression;
    void *handlers;
    struct banner *banner;
    pthread_barrier_t *end_barrier;
};

//static int slave_loadvm_state(void) {
    /*
     * receive memory and disk data
     */
//    return -1;
//}


extern void slave_process_incoming_migration(QEMUFile *f, void * loadvm_handlers, struct banner *banner, int fd, int compression, Byte *decomp_buf, Byte *decomped_buf);

void *start_dest_slave(void *data) {
    struct dest_slave_para * para = (struct dest_slave_para *)data;

    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    int fd;
    int con_fd;
    int val;
    QEMUFile *f;

    if (parse_host_port(&addr, para->listen_ip) < 0) {
        fprintf(stderr, "invalid host/port combination: %s\n", para->listen_ip);
        return NULL;
    }

    DPRINTF("Start slave on dest %s\n", para->listen_ip);
    /*
     * create connection
     */
    fd = qemu_socket(AF_INET, SOCK_STREAM, 0);
    if (fd == -1) {
        fprintf(stderr, "socket error %d\n", socket_error());
        return NULL;
    }

    if(para->compression){
        decomp_buf = malloc(COMPRESS_BUFSIZE);
        decomped_buf = malloc(COMPRESS_BUFSIZE);
        decomp_ptr = decomp_buf;
        decomped_ptr = decomped_buf;
    }

    val = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char *)&val, sizeof(val));

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == -1)
        goto err;

    if (listen(fd, 1) == -1)
        goto err;

    do {
        con_fd = qemu_accept(fd, (struct sockaddr *)&addr, &addrlen);
    } while (con_fd == -1 && socket_error() == EINTR);

    /*
     * wait for further commands
     */
    DPRINTF("accepted migration %d\n", con_fd);

    if (con_fd == -1) {
        fprintf(stderr, "could not accept migration connection\n");
        goto err;
    }

    if (para->ssl_type == SSL_NO)
        f = qemu_fopen_socket(con_fd);
    else if (para->ssl_type == SSL_STRONG)
        f = qemu_fopen_socket_ssl(con_fd);
    else {
        fprintf(stderr, "wrong ssl type use defult ssl solution");
        f = qemu_fopen_socket(con_fd);
    }
            
    if (f == NULL) {
        fprintf(stderr, "could not qemu_fopen socket\n");
        goto err2;
    }

    DPRINTF("DEST slave connection created %s\n", para->listen_ip);
    free(para->listen_ip);

    /*
     * slave handle incoming data
     */
    slave_process_incoming_migration(f, para->handlers, para->banner, con_fd, para->compression, decomp_buf, decomped_buf);

    pthread_barrier_wait(para->end_barrier);    
    DPRINTF("Dest slave end\n");
    //slave_loadvm_state();

    if(para->compression){
        free(decomp_ptr);
        free(decomped_ptr);        
    }
 err2:
    close(con_fd);
 err:
    close(fd);

    free(para);
    return NULL;
}
pthread_t create_dest_slave(char *listen_ip, int ssl_type, int compression, void *loadvm_handlers, 
                            struct banner *banner, pthread_barrier_t *end_barrier);

pthread_t create_dest_slave(char *listen_ip, int ssl_type, int compression, void *loadvm_handlers, 
                            struct banner *banner, pthread_barrier_t *end_barrier) {
    struct dest_slave_para *data = (struct dest_slave_para *)malloc(sizeof(struct dest_slave_para));
    pthread_t tid;
    DPRINTF("create slave %s\n", listen_ip);

    data->listen_ip = listen_ip;
    data->ssl_type = ssl_type;
    data->compression = compression;
    data->handlers = loadvm_handlers;
    data->banner = banner;
    data->end_barrier = end_barrier;
    pthread_create(&tid, NULL, start_dest_slave, data);

    return tid;
}

