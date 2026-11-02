#define _POSIX_C_SOURCE 200809L
#include "core.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define WORKERS 4
#define QUEUE_SIZE 64
#define HEADER_LIMIT 8192
#define BODY_LIMIT 4096
#define ACTIVE_SQL "status IN ('Waiting','Reserved','Ready','Running')"
static const int capacity[3] = {1,2,1};
static const int claims[3][3] = {{0,1,1},{1,0,1},{0,1,1}};
static const char *resources[3] = {"3D printer","Workbench","Technician"};
static const char *stages[3] = {"Preparation","Fabrication","Inspection"};
static const char *db_path = "data/makerlab-c.db";
static const char *static_dir = "static";
static int listening_fd = -1;
static volatile sig_atomic_t interrupted = 0;
static int stopping = 0; /* Accessed under queue_mutex. */
static int pending[QUEUE_SIZE], q_head, q_size;
static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready = PTHREAD_COND_INITIALIZER;

typedef struct { char *data; size_t length, capacity; int failed; } Buffer;
static double now_seconds(void) { struct timeval t; gettimeofday(&t,NULL); return t.tv_sec + t.tv_usec/1000000.0; }
static void append(Buffer *b, const char *format, ...) {
    if (b->failed) return;
    va_list ap; va_start(ap,format); va_list copy; va_copy(copy,ap);
    int n = vsnprintf(NULL,0,format,copy); va_end(copy);
    if (n < 0) { b->failed=1; va_end(ap); return; }
    size_t needed=b->length+(size_t)n+1;
    if (needed>b->capacity) {
        size_t cap=needed*2; char *p=realloc(b->data,cap);
        if (!p) { b->failed=1; va_end(ap); return; }
        b->data=p; b->capacity=cap;
    }
    vsnprintf(b->data+b->length,b->capacity-b->length,format,ap);
    va_end(ap); b->length+=(size_t)n;
}
static void json_string(Buffer *b, const char *s) {
    append(b,"\"");
    for (const unsigned char *p=(const unsigned char *)(s?s:""); *p; p++) {
        if (*p=='"' || *p=='\\') append(b,"\\%c",*p);
        else if (*p<32) append(b,"\\u%04x",*p);
        else append(b,"%c",*p);
    }
    append(b,"\"");
}
static void vector(Buffer *b, const int v[3]) { append(b,"[%d,%d,%d]",v[0],v[1],v[2]); }
static int execute(sqlite3 *db, const char *sql) { return sqlite3_exec(db,sql,NULL,NULL,NULL)==SQLITE_OK; }
static sqlite3_stmt *prepare(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s=NULL;
    if (sqlite3_prepare_v2(db,sql,-1,&s,NULL)!=SQLITE_OK) return NULL;
    return s;
}
static int finish(sqlite3_stmt *s) { if (!s) return 0; int ok=sqlite3_step(s)==SQLITE_DONE; sqlite3_finalize(s); return ok; }
static sqlite3 *open_db(void) {
    sqlite3 *db=NULL;
    if (sqlite3_open(db_path,&db)!=SQLITE_OK) { sqlite3_close(db); return NULL; }
    sqlite3_busy_timeout(db,5000);
    return db;
}
static int log_event(sqlite3 *db, const char *kind, const char *message) {
    sqlite3_stmt *s=prepare(db,"INSERT INTO events(at,kind,message) VALUES(?,?,?)");
    if (!s) return 0;
    sqlite3_bind_double(s,1,now_seconds()); sqlite3_bind_text(s,2,kind,-1,SQLITE_TRANSIENT); sqlite3_bind_text(s,3,message,-1,SQLITE_TRANSIENT);
    return finish(s);
}
static int load_active(sqlite3 *db, Job *jobs, size_t *count, int available[3]) {
    sqlite3_stmt *s=prepare(db,"SELECT id,priority,queued_at,stage,p,b,t,status FROM jobs WHERE " ACTIVE_SQL " ORDER BY id");
    if (!s) return 0;
    *count=0; memcpy(available,capacity,sizeof(capacity)); int rc;
    while ((rc=sqlite3_step(s))==SQLITE_ROW) {
        if (*count>=MAX_ACTIVE) { sqlite3_finalize(s); return 0; }
        Job *j=&jobs[(*count)++]; memset(j,0,sizeof(*j));
        j->id=sqlite3_column_int64(s,0); j->priority=sqlite3_column_int(s,1); j->queued_at=sqlite3_column_double(s,2);
        int stage=sqlite3_column_int(s,3);
        if (stage<0 || stage>2) { sqlite3_finalize(s); return 0; }
        const char *status=(const char *)sqlite3_column_text(s,7);
        j->eligible=!strcmp(status,"Waiting") || !strcmp(status,"Reserved");
        for (int r=0;r<3;r++) { j->maximum[r]=claims[stage][r]; j->allocation[r]=sqlite3_column_int(s,4+r); available[r]-=j->allocation[r]; }
    }
    sqlite3_finalize(s); return rc==SQLITE_DONE;
}
static int snapshot(sqlite3 *db, Buffer *out) {
    if (!execute(db,"BEGIN")) return 0;
    Job jobs[MAX_ACTIVE]; size_t count, sequence[MAX_ACTIVE]; int available[3];
    if (!load_active(db,jobs,&count,available)) return 0;
    int safe=banker_safe(available,jobs,count,sequence);
    if (safe<0) return 0;
    append(out,"{\"resources\":[\"3D printer\",\"Workbench\",\"Technician\"],\"capacity\":[1,2,1],\"available\":");
    vector(out,available); append(out,",\"safe_sequence\":");
    if (!safe) append(out,"null");
    else { append(out,"["); for(size_t i=0;i<count;i++) append(out,"%s%lld",i?",":"",jobs[sequence[i]].id); append(out,"]"); }
    double now=now_seconds(); append(out,",\"server_time\":%.6f,\"jobs\":[",now);
    sqlite3_stmt *s=prepare(db,"SELECT id,title,owner,priority,stage,status,queued_at,created_at,started_at,finished_at,p,b,t FROM jobs ORDER BY id");
    if (!s) return 0;
    int first=1,rc;
    while ((rc=sqlite3_step(s))==SQLITE_ROW) {
        int stage=sqlite3_column_int(s,4); if (stage<0 || stage>2) { sqlite3_finalize(s); return 0; }
        const char *status=(const char *)sqlite3_column_text(s,5);
        int active=strcmp(status,"Completed") && strcmp(status,"Cancelled");
        int max[3]={0},held[3],need[3];
        for (int r=0;r<3;r++) { max[r]=active?claims[stage][r]:0; held[r]=sqlite3_column_int(s,10+r); need[r]=max[r]-held[r]; }
        Job job={.priority=sqlite3_column_int(s,3),.queued_at=sqlite3_column_double(s,6)};
        int priority=(!strcmp(status,"Waiting") || !strcmp(status,"Reserved"))?effective_priority(&job,now):job.priority;
        append(out,"%s{\"id\":%lld,\"title\":",first?"":",",(long long)sqlite3_column_int64(s,0)); first=0;
        json_string(out,(const char *)sqlite3_column_text(s,1)); append(out,",\"owner\":"); json_string(out,(const char *)sqlite3_column_text(s,2));
        append(out,",\"priority\":%d,\"effective_priority\":%d,\"stage\":%d,\"stage_name\":",job.priority,priority,stage); json_string(out,stages[stage]);
        append(out,",\"status\":"); json_string(out,status);
        append(out,",\"queued_at\":%.6f,\"created_at\":%.6f",job.queued_at,sqlite3_column_double(s,7));
        for (int c=8;c<=9;c++) { append(out,",\"%s\":",c==8?"started_at":"finished_at"); if(sqlite3_column_type(s,c)==SQLITE_NULL) append(out,"null"); else append(out,"%.6f",sqlite3_column_double(s,c)); }
        append(out,",\"maximum\":");vector(out,max);append(out,",\"allocation\":");vector(out,held);append(out,",\"need\":");vector(out,need);append(out,"}");
    }
    sqlite3_finalize(s); if(rc!=SQLITE_DONE) return 0;
    append(out,"],\"events\":[");
    s=prepare(db,"SELECT id,at,kind,message FROM events ORDER BY id DESC LIMIT 80"); if(!s)return 0; first=1;
    while ((rc=sqlite3_step(s))==SQLITE_ROW) {
        append(out,"%s{\"id\":%lld,\"at\":%.6f,\"kind\":",first?"":",",(long long)sqlite3_column_int64(s,0),sqlite3_column_double(s,1));first=0;
        json_string(out,(const char *)sqlite3_column_text(s,2));append(out,",\"message\":");json_string(out,(const char *)sqlite3_column_text(s,3));append(out,"}");
    }
    sqlite3_finalize(s);append(out,"]}");return rc==SQLITE_DONE && !out->failed && execute(db,"COMMIT");
}
static int valid_json_object(sqlite3 *db,const char *body) {
    sqlite3_stmt *s=prepare(db,"SELECT json_valid(?)");if(!s)return 0;
    sqlite3_bind_text(s,1,body,-1,SQLITE_TRANSIENT);int ok=sqlite3_step(s)==SQLITE_ROW && sqlite3_column_int(s,0);sqlite3_finalize(s);if(!ok)return 0;
    s=prepare(db,"SELECT json_type(?)='object'");if(!s)return 0;
    sqlite3_bind_text(s,1,body,-1,SQLITE_TRANSIENT);ok=sqlite3_step(s)==SQLITE_ROW && sqlite3_column_int(s,0);sqlite3_finalize(s);return ok;
}
static int create_job(sqlite3 *db,const char *body,char *message,size_t size,long long *id) {
    sqlite3_stmt *s=prepare(db,"SELECT json_type(?1,'$.title'),json_extract(?1,'$.title'),json_type(?1,'$.owner'),json_extract(?1,'$.owner'),json_type(?1,'$.priority'),json_extract(?1,'$.priority')");
    if(!s)return 503;
    sqlite3_bind_text(s,1,body,-1,SQLITE_TRANSIENT);
    if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);return 503;}
    char title[321]={0},owner[241]={0};int priority=5,ok=1;
    for(int field=0;field<2;field++) {
        const char *type=(const char *)sqlite3_column_text(s,field*2);
        const char *text=(const char *)sqlite3_column_text(s,field*2+1);
        int bytes=sqlite3_column_bytes(s,field*2+1),limit=field==0?320:240;
        if(!type || strcmp(type,"text") || !text || bytes<1 || bytes>limit || memchr(text,0,(size_t)bytes)){ok=0;break;}
        for(int i=0;i<bytes;i++)if((unsigned char)text[i]<32)ok=0;
        char *dest=field==0?title:owner;memcpy(dest,text,(size_t)bytes);
        char *start=dest;while(isspace((unsigned char)*start))start++;memmove(dest,start,strlen(start)+1);
        size_t len=strlen(dest);while(len && isspace((unsigned char)dest[len-1]))dest[--len]=0;
        if(!len)ok=0;
        size_t chars=0;for(size_t i=0;i<len;i++)if(((unsigned char)dest[i]&0xC0)!=0x80)chars++;
        if(chars>(field==0?80u:60u))ok=0;
    }
    const char *ptype=(const char *)sqlite3_column_text(s,4);
    if(ptype){sqlite3_int64 p=sqlite3_column_int64(s,5);if(strcmp(ptype,"integer")||p<1||p>10)ok=0;else priority=(int)p;}
    sqlite3_finalize(s);
    if(!ok){snprintf(message,size,"Use a title (1-80 characters), a team (1-60), and integer priority 1-10; control characters are not allowed.");return 400;}
    Job jobs[MAX_ACTIVE];size_t count;int available[3];if(!load_active(db,jobs,&count,available))return 503;
    if(count==MAX_ACTIVE){snprintf(message,size,"The lab supports at most 100 active projects.");return 400;}
    s=prepare(db,"INSERT INTO jobs(title,owner,priority,queued_at,created_at) VALUES(?,?,?,?,?)");if(!s)return 503;
    sqlite3_bind_text(s,1,title,-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,owner,-1,SQLITE_TRANSIENT);sqlite3_bind_int(s,3,priority);
    double now=now_seconds();sqlite3_bind_double(s,4,now);sqlite3_bind_double(s,5,now);if(!finish(s))return 503;
    *id=sqlite3_last_insert_rowid(db);snprintf(message,size,"J%03lld: %s entered Preparation",*id,title);
    return log_event(db,"Created",message)?200:503;
}
static int dispatch(sqlite3 *db,char *message,size_t size) {
    Job jobs[MAX_ACTIVE];size_t count,order[MAX_ACTIVE],length,sequence[MAX_ACTIVE];int available[3];
    if(!load_active(db,jobs,&count,available))return 503;
    priority_order(jobs,count,now_seconds(),order,&length);
    for(size_t k=0;k<length;k++) {
        Job *j=&jobs[order[k]];
        for(int r=0;r<3;r++) {
            if(j->maximum[r]==j->allocation[r] || available[r]==0)continue;
            j->allocation[r]++;available[r]--;
            int safe=banker_safe(available,jobs,count,sequence);
            if(safe!=1) {
                j->allocation[r]--;available[r]++;
                if(safe<0)return 503;
                snprintf(message,size,"J%03lld: %s withheld; tentative grant has no safe completion sequence.",j->id,resources[r]);
                if(!log_event(db,"Unsafe",message))return 503;
                continue;
            }
            int ready=1;for(int c=0;c<3;c++)if(j->allocation[c]!=j->maximum[c])ready=0;
            sqlite3_stmt *s=prepare(db,"UPDATE jobs SET p=?,b=?,t=?,status=? WHERE id=?");if(!s)return 503;
            for(int c=0;c<3;c++)sqlite3_bind_int(s,c+1,j->allocation[c]);
            sqlite3_bind_text(s,4,ready?"Ready":"Reserved",-1,SQLITE_STATIC);sqlite3_bind_int64(s,5,j->id);if(!finish(s))return 503;
            snprintf(message,size,"J%03lld: reserved %s; a safe completion sequence remains.",j->id,resources[r]);
            return log_event(db,"Granted",message)?200:503;
        }
    }
    snprintf(message,size,"No resource granted. Start a ready stage or complete a running stage to release resources.");
    return log_event(db,"Waiting",message)?200:503;
}
static int action(sqlite3 *db,long long id,const char *name,char *message,size_t size) {
    sqlite3_stmt *s=prepare(db,"SELECT stage,status FROM jobs WHERE id=?");if(!s)return 503;
    sqlite3_bind_int64(s,1,id);
    if(sqlite3_step(s)!=SQLITE_ROW){sqlite3_finalize(s);snprintf(message,size,"Project not found.");return 400;}
    int stage=sqlite3_column_int(s,0);char status[20];snprintf(status,sizeof(status),"%s",sqlite3_column_text(s,1));sqlite3_finalize(s);
    double now=now_seconds();const char *kind;
    if(!strcmp(name,"start")) {
        if(strcmp(status,"Ready")){snprintf(message,size,"Reserve the full stage claim before starting.");return 400;}
        s=prepare(db,"UPDATE jobs SET status='Running',started_at=? WHERE id=?");if(!s)return 503;
        sqlite3_bind_double(s,1,now);sqlite3_bind_int64(s,2,id);kind="Start";
        snprintf(message,size,"J%03lld: %s started.",id,stages[stage]);
    } else if(!strcmp(name,"complete")) {
        if(strcmp(status,"Running")){snprintf(message,size,"Only a running stage can complete.");return 400;}
        s=prepare(db,"UPDATE jobs SET stage=?,status=?,p=0,b=0,t=0,queued_at=?,started_at=NULL,finished_at=? WHERE id=?");if(!s)return 503;
        sqlite3_bind_int(s,1,stage==2?2:stage+1);sqlite3_bind_text(s,2,stage==2?"Completed":"Waiting",-1,SQLITE_STATIC);
        sqlite3_bind_double(s,3,now);if(stage==2)sqlite3_bind_double(s,4,now);else sqlite3_bind_null(s,4);sqlite3_bind_int64(s,5,id);kind="Complete";
        snprintf(message,size,"J%03lld: %s completed; resources released.",id,stages[stage]);
    } else if(!strcmp(name,"cancel")) {
        if(!strcmp(status,"Completed")||!strcmp(status,"Cancelled")){snprintf(message,size,"Project is already closed.");return 400;}
        s=prepare(db,"UPDATE jobs SET status='Cancelled',p=0,b=0,t=0,finished_at=? WHERE id=?");if(!s)return 503;
        sqlite3_bind_double(s,1,now);sqlite3_bind_int64(s,2,id);kind="Cancel";
        snprintf(message,size,"J%03lld: cancelled; all resources released.",id);
    } else {snprintf(message,size,"Unknown action.");return 400;}
    if(!finish(s))return 503;
    return log_event(db,kind,message)?200:503;
}
static int demo(sqlite3 *db,char *message,size_t size) {
    Job jobs[MAX_ACTIVE];size_t count;int available[3];if(!load_active(db,jobs,&count,available))return 503;
    if(count){snprintf(message,size,"Complete or cancel active projects before loading the demo. History is preserved.");return 400;}
    long long ids[2];
    for(int i=0;i<2;i++) {
        sqlite3_stmt *s=prepare(db,"INSERT INTO jobs(title,owner,priority,stage,status,queued_at,created_at,p) VALUES(?,'Demo team',?,1,?,?,?,?)");if(!s)return 503;
        sqlite3_bind_text(s,1,i?"Drone bracket":"Robotics enclosure",-1,SQLITE_STATIC);sqlite3_bind_int(s,2,i?9:2);sqlite3_bind_text(s,3,i?"Waiting":"Reserved",-1,SQLITE_STATIC);
        double now=now_seconds();sqlite3_bind_double(s,4,now);sqlite3_bind_double(s,5,now);sqlite3_bind_int(s,6,i?0:1);if(!finish(s))return 503;ids[i]=sqlite3_last_insert_rowid(db);
    }
    snprintf(message,size,"J%03lld holds the printer. J%03lld has higher priority. Dispatch next to see its unsafe technician request rejected.",ids[0],ids[1]);
    return log_event(db,"Demo",message)?200:503;
}
static int send_all(int fd,const char *data,size_t len) {
    while(len){ssize_t n=send(fd,data,len,0);if(n<0 && errno==EINTR)continue;if(n<=0)return 0;data+=n;len-=(size_t)n;}return 1;
}
static void respond(int fd,int status,const char *type,const char *body,size_t len) {
    char head[800];
    int n=snprintf(head,sizeof(head),"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nContent-Security-Policy: default-src 'self'; style-src 'self'; script-src 'self'; frame-ancestors 'none'\r\n\r\n",status,status==200?"OK":status==400?"Bad Request":status==403?"Forbidden":status==404?"Not Found":"Service Unavailable",type,len);
    if(n>0 && (size_t)n<sizeof(head) && send_all(fd,head,(size_t)n))send_all(fd,body,len);
}
static void json_response(int fd,int status,const char *key,const char *message,long long id) {
    Buffer out={0};append(&out,"{\"%s\":",key);json_string(&out,message);if(id)append(&out,",\"id\":%lld",id);append(&out,"}");
    if(!out.failed)respond(fd,status,"application/json; charset=utf-8",out.data,out.length);free(out.data);
}
static void serve_file(int fd,const char *name,const char *mime) {
    char path[2048];int n=snprintf(path,sizeof(path),"%s/%s",static_dir,name);
    if(n<0 || (size_t)n>=sizeof(path)){json_response(fd,503,"error","Static path too long.",0);return;}
    FILE *file=fopen(path,"rb");if(!file){json_response(fd,404,"error","Asset missing; launch from the project folder.",0);return;}
    Buffer data={0};char chunk[4097];size_t got;
    while((got=fread(chunk,1,4096,file))>0){chunk[got]=0;append(&data,"%s",chunk);}int failed=ferror(file)||data.failed;fclose(file);
    if(failed)json_response(fd,503,"error","Cannot read asset.",0);else respond(fd,200,mime,data.data?data.data:"",data.length);free(data.data);
}
static void handle(int fd) {
    struct timeval timeout={5,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    char raw[HEADER_LIMIT+BODY_LIMIT+1];size_t used=0;char *end=NULL;
    while(!end && used<HEADER_LIMIT) {
        ssize_t got=recv(fd,raw+used,HEADER_LIMIT-used,0);if(got<=0)return;
        if(memchr(raw+used,0,(size_t)got))return;used+=(size_t)got;raw[used]=0;end=strstr(raw,"\r\n\r\n");
    }
    if(!end){json_response(fd,400,"error","Request headers too large.",0);return;}
    size_t header_bytes=(size_t)(end-raw)+4;*end=0;
    char method[12],path[512],version[20],extra;
    char *line_end=strstr(raw,"\r\n");if(!line_end)return;*line_end=0;
    if(sscanf(raw,"%11s %511s %19s %c",method,path,version,&extra)!=3 || strcmp(version,"HTTP/1.1")){json_response(fd,400,"error","Use HTTP/1.1.",0);return;}
    char *host=NULL,*origin=NULL,*content_type=NULL,*marker=NULL;long length=0;int length_seen=0,bad=0;
    char *line=line_end+2;
    while(*line) {
        char *next=strstr(line,"\r\n");if(next)*next=0;
        char *colon=strchr(line,':');if(!colon){bad=1;break;}*colon=0;
        char *value=colon+1;while(*value==' '||*value=='\t')value++;
        size_t len=strlen(value);while(len && (value[len-1]==' '||value[len-1]=='\t'))value[--len]=0;
        if(!strcasecmp(line,"Content-Length")) {
            if(length_seen++ || !*value)bad=1;
            for(char *p=value;*p;p++)if(!isdigit((unsigned char)*p))bad=1;
            char *tail;errno=0;length=strtol(value,&tail,10);if(errno||*tail||length<0||length>BODY_LIMIT)bad=1;
        } else if(!strcasecmp(line,"Transfer-Encoding"))bad=1;
        else if(!strcasecmp(line,"Host")){if(host)bad=1;host=value;}
        else if(!strcasecmp(line,"Origin")){if(origin)bad=1;origin=value;}
        else if(!strcasecmp(line,"Content-Type")){if(content_type)bad=1;content_type=value;}
        else if(!strcasecmp(line,"X-MakerLab")){if(marker)bad=1;marker=value;}
        if(!next)break;line=next+2;
    }
    if(bad||!host){json_response(fd,400,"error","Malformed or oversized request.",0);return;}
    char hostname[256];snprintf(hostname,sizeof(hostname),"%s",host);char *port=strchr(hostname,':');if(port)*port=0;
    if(strcmp(hostname,"127.0.0.1") && strcmp(hostname,"localhost")){json_response(fd,403,"error","This classroom server accepts localhost requests only.",0);return;}
    if(!strcmp(method,"GET")) {
        if(!strcmp(path,"/")){serve_file(fd,"index.html","text/html; charset=utf-8");return;}
        if(!strcmp(path,"/style.css")){serve_file(fd,"style.css","text/css; charset=utf-8");return;}
        if(!strcmp(path,"/app.js")){serve_file(fd,"app.js","text/javascript; charset=utf-8");return;}
        if(!strcmp(path,"/api/health")){json_response(fd,200,"status","ok",0);return;}
        if(!strcmp(path,"/api/state")) {
            sqlite3 *db=open_db();Buffer out={0};int ok=db && snapshot(db,&out);
            if(ok)respond(fd,200,"application/json; charset=utf-8",out.data,out.length);else json_response(fd,503,"error","Database unavailable.",0);
            free(out.data);sqlite3_close(db);return;
        }
        json_response(fd,404,"error","Not found.",0);return;
    }
    char expected_origin[300];snprintf(expected_origin,sizeof(expected_origin),"http://%s",host);
    if(strcmp(method,"POST")||!marker||strcmp(marker,"1")||!content_type||strcmp(content_type,"application/json")||(origin && strcmp(origin,expected_origin))) {
        json_response(fd,403,"error","Use the local MakerLab dashboard.",0);return;
    }
    if(!length_seen || length==0){json_response(fd,400,"error","A JSON object is required.",0);return;}
    size_t target=header_bytes+(size_t)length;
    while(used<target){ssize_t got=recv(fd,raw+used,target-used,0);if(got<=0)return;used+=(size_t)got;}
    char *body=raw+header_bytes;if(memchr(body,0,(size_t)length)){json_response(fd,400,"error","Invalid JSON.",0);return;}raw[target]=0;
    sqlite3 *db=open_db();if(!db){json_response(fd,503,"error","Database unavailable.",0);return;}
    if(!valid_json_object(db,body)){json_response(fd,400,"error","Expected a valid JSON object.",0);sqlite3_close(db);return;}
    char message[1024]="Database busy or write failed; try again.";int status=503;long long id=0;
    if(execute(db,"BEGIN IMMEDIATE")) {
        if(!strcmp(path,"/api/jobs"))status=create_job(db,body,message,sizeof(message),&id);
        else if(!strcmp(path,"/api/dispatch"))status=dispatch(db,message,sizeof(message));
        else if(!strcmp(path,"/api/demo"))status=demo(db,message,sizeof(message));
        else {
            char name[24];long long job_id;int consumed=0;
            if(sscanf(path,"/api/jobs/%lld/%23[a-z]%n",&job_id,name,&consumed)==2 && path[consumed]==0 && job_id>0)status=action(db,job_id,name,message,sizeof(message));
            else{status=404;snprintf(message,sizeof(message),"Not found.");}
        }
        if(status==200 && !execute(db,"COMMIT"))status=503;
        if(status!=200)execute(db,"ROLLBACK");
    }
    if(status==503)snprintf(message,sizeof(message),"Database busy or write failed; try again.");
    sqlite3_close(db);json_response(fd,status,status==200?"message":"error",message,status==200?id:0);
}
static void *worker(void *unused) {
    (void)unused;
    for(;;) {
        pthread_mutex_lock(&queue_mutex);
        while(!q_size && !stopping)pthread_cond_wait(&queue_ready,&queue_mutex);
        if(!q_size && stopping){pthread_mutex_unlock(&queue_mutex);return NULL;}
        int fd=pending[q_head];q_head=(q_head+1)%QUEUE_SIZE;q_size--;
        pthread_mutex_unlock(&queue_mutex);handle(fd);close(fd);
    }
}
static void stop_server(int sig) { (void)sig;interrupted=1; }
int main(int argc,char **argv) {
    int port=8080;const char *host="127.0.0.1";
    const char *env=getenv("MAKERLAB_DB");if(env && *env)db_path=env;
    for(int i=1;i<argc;i++) {
        if(i+1>=argc){fprintf(stderr,"Usage: %s [--host 127.0.0.1|0.0.0.0] [--port 8080] [--db path] [--static static]\n",argv[0]);return 1;}
        if(!strcmp(argv[i],"--port")){char *end;long p=strtol(argv[++i],&end,10);if(*end||p<1||p>65535)return 1;port=(int)p;}
        else if(!strcmp(argv[i],"--host"))host=argv[++i];
        else if(!strcmp(argv[i],"--db"))db_path=argv[++i];
        else if(!strcmp(argv[i],"--static"))static_dir=argv[++i];
        else{fprintf(stderr,"Unknown option: %s\n",argv[i]);return 1;}
    }
    if(strcmp(host,"127.0.0.1") && strcmp(host,"0.0.0.0")){fprintf(stderr,"Unsupported bind address.\n");return 1;}
    mkdir("data",0700);
    sqlite3 *db=open_db();
    const char *schema="PRAGMA journal_mode=WAL;CREATE TABLE IF NOT EXISTS jobs(id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,owner TEXT NOT NULL,priority INTEGER NOT NULL CHECK(priority BETWEEN 1 AND 10),stage INTEGER NOT NULL DEFAULT 0 CHECK(stage BETWEEN 0 AND 2),status TEXT NOT NULL DEFAULT 'Waiting' CHECK(status IN ('Waiting','Reserved','Ready','Running','Completed','Cancelled')),queued_at REAL NOT NULL,created_at REAL NOT NULL,started_at REAL,finished_at REAL,p INTEGER NOT NULL DEFAULT 0 CHECK(p BETWEEN 0 AND 1),b INTEGER NOT NULL DEFAULT 0 CHECK(b BETWEEN 0 AND 1),t INTEGER NOT NULL DEFAULT 0 CHECK(t BETWEEN 0 AND 1));CREATE TABLE IF NOT EXISTS events(id INTEGER PRIMARY KEY AUTOINCREMENT,at REAL NOT NULL,kind TEXT NOT NULL,message TEXT NOT NULL);";
    if(!db||!execute(db,schema)||!valid_json_object(db,"{}")){fprintf(stderr,"Database initialization failed; SQLite JSON functions are required.\n");sqlite3_close(db);return 1;}sqlite3_close(db);
    signal(SIGPIPE,SIG_IGN);signal(SIGINT,stop_server);signal(SIGTERM,stop_server);
    listening_fd=socket(AF_INET,SOCK_STREAM,0);int reuse=1;setsockopt(listening_fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));
    struct sockaddr_in address={0};address.sin_family=AF_INET;address.sin_port=htons((unsigned short)port);inet_pton(AF_INET,host,&address.sin_addr);
    if(bind(listening_fd,(struct sockaddr *)&address,sizeof(address))||listen(listening_fd,64)){perror("Cannot listen");return 1;}
    pthread_t threads[WORKERS];
    for(int i=0;i<WORKERS;i++)if(pthread_create(&threads[i],NULL,worker,NULL)){fprintf(stderr,"Cannot create worker.\n");return 1;}
    printf("MakerLabOS C server: http://127.0.0.1:%d (%d pthread workers)\n",port,WORKERS);fflush(stdout);
    while(!interrupted) {
        struct pollfd ready={.fd=listening_fd,.events=POLLIN};
        int result=poll(&ready,1,250);
        if(result<0){if(errno==EINTR)continue;perror("poll");break;}
        if(!result)continue;
        if(!(ready.revents&POLLIN))break;
        int fd=accept(listening_fd,NULL,NULL);if(fd<0){if(errno==EINTR)continue;perror("accept");break;}
        pthread_mutex_lock(&queue_mutex);
        if(q_size==QUEUE_SIZE){pthread_mutex_unlock(&queue_mutex);close(fd);continue;}
        pending[(q_head+q_size)%QUEUE_SIZE]=fd;q_size++;pthread_cond_signal(&queue_ready);pthread_mutex_unlock(&queue_mutex);
    }
    close(listening_fd);
    pthread_mutex_lock(&queue_mutex);stopping=1;pthread_cond_broadcast(&queue_ready);pthread_mutex_unlock(&queue_mutex);
    for(int i=0;i<WORKERS;i++)pthread_join(threads[i],NULL);
    return 0;
}
