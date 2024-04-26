#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <cstddef>
// #include <error.h>
#include <errno.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <sys/types.h>
#include <assert.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

#include <spdlog/spdlog.h>
#include "Server.h"

struct FdInfo
{
    int fd;
    int epfd;
    pthread_t tid;
};


int initListenFd(unsigned short port)
{
    // 1.创建监听的fd
    int lfd = socket(AF_INET,SOCK_STREAM,0);
    if(lfd == -1)
    {
        perror("socket");
        return -1;
    }
    // 2.设置端口复用
    int opt = 1;
    int ret = setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));

    // 3.绑定
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(lfd,(struct sockaddr*)&addr,sizeof(addr));
    if(ret == -1)
    {
        perror("bind error");
        return -1;
    }

    // 4.设置监听
    ret = listen(lfd,128);
    if(ret == -1)
    {
        perror("listen error");
        return -1;
    }

    // 5.返回fd
    return lfd;
}

int epollRun(int lfd)
{
    //1.创建epoll实例
    int epfd = epoll_create(1);
    if(epfd == -1)
    {
        perror("epoll_create");
        return -1;
    }
    //添加到树上，
    struct epoll_event ev;
    ev.data.fd = lfd;
    ev.events = EPOLLIN;
    int ret = epoll_ctl(epfd,EPOLL_CTL_ADD,lfd,&ev);
    if(ret == -1)
    {
        perror("epoll_ctl");
        return -1;
    }
    //3.检测
    struct epoll_event evs[1024];
    while(1)
    {
        //-1表示阻塞，0表示不阻塞
        int num = epoll_wait(epfd,evs,sizeof(evs)/sizeof(struct epoll_event),-1);
        for(int i = 0;i< num;++i){
            struct FdInfo* info = (struct FdInfo*)malloc(sizeof(struct FdInfo));
            int fd = evs[i].data.fd;
            info->epfd = epfd;
            info->fd = fd;
            // int fd = evs[i].data.fd;
            if(fd == lfd){
                //建立新连接 accept
                // acceptClient(lfd,epfd);
                pthread_create(&info->tid,NULL,&acceptClient,info);
            }else{
                //主要接收对端数据
                // recvHttpResquest(fd,epfd); 
                pthread_create(&info->tid,NULL,&recvHttpResquest,info);

            }
        }
    }
    return 0;
}

void* acceptClient(void* arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;
    int lfd = info->fd;
    int epfd = info->epfd;
    //建立连接
    int cfd = accept(lfd,NULL,NULL);
    if(cfd == -1)
    {
        perror("accept");
        return NULL;
    }
    //设置非阻塞
    int flag = fcntl(cfd,F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(cfd,F_SETFL,flag);

    //cfd添加到epoll描述符里
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET;
    int ret = epoll_ctl(epfd,EPOLL_CTL_ADD,cfd,&ev);
    if(ret == -1)
    {
        perror("cfd add error");
        return NULL;
    }
    spdlog::info("accept client thread id {}",info->tid);
    return 0;
}
void* recvHttpResquest(void* arg)
{
    struct FdInfo* info = (struct FdInfo*)arg;
    int cfd = info->fd;
    int epfd = info->epfd;

    int len = 0,total = 0;
    char tmp[1024] = {0};
    char buf[4096] = {0};
    while((len = recv(cfd,tmp,sizeof tmp,0))>0)
    {
        if(total + len < sizeof buf){
            memcpy(buf + total,tmp,len);
        }
        total += len;
    }
    //判断是否接收完毕
    if(len == -1 && errno == EAGAIN)
    {
        //解析请求行
        char* pt = strstr(buf,"\r\n");
        int reqLen = pt - buf;
        buf[reqLen] = '\0';
        parseRequestLine(buf,cfd);

    }else if(len == 0){
        //客服端断开连接
        epoll_ctl(epfd,EPOLL_CTL_DEL,cfd,NULL);
        close(cfd);
    }
    else{
        perror("recv error");
        return NULL;
    }
    spdlog::info("receive message thread id {}",info->tid);
    return 0;
}

int parseRequestLine(const char* line,int cfd)
{
    //解析请求行get /xxx/1.jpg http/1.1
    char method[12];
    char path[1024];
    sscanf(line,"%[^ ] %[^ ]",method,path);
    if(strcasecmp(method,"get") != 0)
    {
        return -1;
    }
    spdlog::info("method:{},path:{}",method,path);
    //处理客户端的请求的静态资源（目录或者文件）
    char* file = NULL;
    if(strcmp(path,"/") == 0)
    {
        file = "./";
    }else{
        file = path + 1;
    }

    struct stat st;
    int ret = stat(file,&st);
    if(ret == -1)
    {
        //文件不存在，--回复404
        sendHeadMsg(cfd,404,"Not Found",getFileType(".html"),-1);
        sendFile("404.html",cfd);
        return 0;
    }
    //判断文件类型
    if(S_ISDIR(st.st_mode))
    {
        //把本地目录的内容发送给客户端
        sendHeadMsg(cfd,200,"OK",getFileType(file),st.st_size);
        sendDir(file,cfd);
    }else{
        //把文件的内容发送给客户端
        sendHeadMsg(cfd,200,"OK",getFileType(file),st.st_size);
        sendFile(file,cfd);
    }
    return 0;
}

int sendFile(const char* fileName,int cfd){
    //1.打开文件
    // spdlog::info("start to send file length {}");
    int fd = open(fileName,O_RDONLY);
    spdlog::info("start to send file length");
    assert(fd > 0);
    char buf[1024];
    #if 0
    while(1)
    {
        int len = read(fd,buf,sizeof buf);
        if(len > 0){
            send(cfd,buf,len,0);
            usleep(10);//这非常重要
        }
        else if(len == 0)
        {
            break;
        }else{
            perror("read");
        }
    }
    #else
    off_t offset=0;
    int size = lseek(fd,0,SEEK_END);
    spdlog::info("size of file{}",size);
    lseek(fd,0,SEEK_SET);
    while(offset < size)
    {
        // spdlog::info("start to send file length");
        int ret = sendfile(cfd,fd,&offset,size-offset);
        // if(ret > 0){
        //     spdlog::info("send file length {}",ret);
        // }else{
        //     spdlog::info("error send file length {}",ret);
        // }
    }
    #endif
    return 0;
}
int sendHeadMsg(int cfd,int status,const char* descr,const char* type,int length)
{
    //状态行
    char buf[4096];
    sprintf(buf,"http/1.1 %d %s\r\n",status,descr);
    //响应头
    sprintf(buf+strlen(buf),"content-type: %s\r\n",type);
    sprintf(buf+strlen(buf),"content-length: %d\r\n\r\n",length);
    spdlog::info(buf);
    send(cfd,buf,strlen(buf),0);
    return 0;
}
const char* getFileType(const char* name)
{
    //a.jpg a.mp4 a.html
    //自左向右查找，.字符，如不存在返回NULL
    const char* dot = strrchr(name,'.');
    if (dot == NULL)
        // return "text/plain; charset=utf-8"; // 纯文本
        return "text/html; charset=utf-8";
    if(strcmp(dot,".cpp") ==0)
        return "text/plain; charset=utf-8";
    if(strcmp(dot,".html") == 0 || strcmp(dot,".htm") == 0)
        return "text/html; charset=utf-8";
    if(strcmp(dot,".jpg") == 0 || strcmp(dot,".jpeg") == 0)
        return "image/jpeg";
    if(strcmp(dot,".gif") == 0)
        return "image/gif";
    if (strcmp(dot,".png")== 0)
        return "image/png";
    if(strcmp(dot,".css")==0)
        return "text/css";
    if (strcmp(dot,".au") == 0)
        return "audio/basic";
    if(strcmp(dot,".wav") == 0)
        return "audio/wav";
    if(strcmp(dot,".avi") == 0)
        return "video/x-msvideo";
    if(strcmp(dot,".mov") == 0 || strcmp(dot,".qt") == 0)
        return "video/quicktime";
    if(strcmp(dot,".mpeq")== 0 || strcmp(dot,".mpe") == 0)
        return "video/mpeg";
    return "text/html; charset=utf-8";
}
int sendDir(const char* dirName,int cfd)
{
    char buf[4096]={0};
    sprintf(buf,"<html><head><title>%s</title></head><body><table>",dirName);
    struct dirent** namelist;
    int num = scandir(dirName,&namelist,NULL,&alphasort);
    #if 1
    for(int i = 0; i < num;++i)
    {
        //取出文件名
        char* name = namelist[i]->d_name;
        struct stat st;
        char subPath[1024]={0};
        sprintf(subPath,"%s/%s",dirName,name);
        stat(subPath,&st);
        if(S_ISDIR(st.st_mode)){
            //a标签<a href="">name</a>
            sprintf(buf+strlen(buf),
            "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>",name,name,st.st_size);

        }else{
            //a标签<a href="">name</a>
            sprintf(buf+strlen(buf),
            "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",name,name,st.st_size);

        }
        send(cfd,buf,strlen(buf),0);
        memset(buf,0,sizeof(buf));
        free(namelist[i]);
    }
    #else 
    sprintf(buf+strlen(buf),"<tr><td>hello world</td></tr>");
            // "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>",name,name,st.st_size);
    #endif
    sprintf(buf + strlen(buf),"</table></body></html>");

    spdlog::info(buf);
    send(cfd,buf,strlen(buf),0);
    free(namelist);
    return 0;
}