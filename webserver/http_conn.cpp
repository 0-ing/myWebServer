#include "http_conn.h"

int http_conn :: m_epollfd = -1;
int http_conn :: m_user_count = 0;


// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 网站的根目录
const char* doc_root = "/home/wensong/webserver/resources";


// 设置文件描述符非阻塞
void setnonblocking(int fd) {
    int old_flag = fcntl(fd, F_GETFL);
    int new_flag = old_flag | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_flag);

}


// 向epoll中添加需要监听的文件描述符
void addfd(int epollfd, int fd, bool one_shot){
    epoll_event event;
    event.data.fd = fd;
    // event.events = EPOLLIN | EPOLLRDHUP; // 默认水平触发， EPOLLIN：有读事件发生， EPOLLRDHUP：有本方关闭读的事件发生，当客服端close(fd)时，服务端会触发这个
                                                                                            // 所以这个用来判断客户端是否关闭连接，要不然需要通过recv返回0来判断
                                                                                            // 客户端是否关闭连接
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP; // 边缘触发  // 注意，千万不要把listenfd设置成边缘触发，会报错
    if (one_shot) {
        event.events | EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event); // 将需要监听的文件描述符fd放到监听对象中epollfd(即交给epollfd去监听fd)

    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);   // 将不需要监听的文件描述符fd从监听对象那儿拿走
    close(fd);
}

// 修改文件描述符, 重置 socket上EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化新接受的客户端的连接
void http_conn :: init(int sockfd, const sockaddr_in& addr){
    m_sockfd = sockfd;
    m_address = addr;

    // 设置端口复用
    // 1.防止服务器重启时之前绑定的端口还没释放 2.程序突然退出而系统没有释放端口
    // 针对的是服务器端socket的time_wait状态
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 添加到epoll对象中
    addfd(m_epollfd, m_sockfd, true);
    m_user_count++;   // 客户端的连接数加一

    init();

}


void http_conn:: init() {
    m_check_state = CHECK_STATE_REQUESTLINE;    // 初始化状态为解析请求首行
    m_checked_index = 0;    
    m_start_line = 0;
    m_read_idx = 0;

    m_method = GET;         // 默认请求方式为GET
    m_url = 0;
    m_version = 0;
    m_linger = false;       // 默认不保持链接  Connection : keep-alive保持连接

    m_write_idx = 0;
    m_host = 0;
    m_content_length = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);

    memset((void*)&m_file_stat, 0x00, sizeof(m_file_stat));   // 将这个状态也清空一下，我靠你文件名都清空了，stat坑定也要清空吧

}

// 关闭连接
void http_conn:: close_conn(){

    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;  // 文件描述符都为-1了，那这个文件描述符也就没用了，为啥，因为文件描述符是从0增大
        m_user_count--;  // 总的客户端的连接数减一
    }
}


// 循环读取客户数据，直到无数据刻度或者对方关闭连接
bool http_conn :: read() {

    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }
    
    // 已经读取到的字节
    int bytes_read = 0;
    while(true) {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        if (bytes_read == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 没有数据了,因为我们是非阻塞的读（因为前面，这个套接字已经被设为非阻塞模式了），当输入缓冲区没有数据时，会报错errno = EAGAIN或者EWOULDBLOCK这两个等价
                break;
            }
            return false;
        }
        else if (bytes_read == 0) {
            // 对方关闭连接
            return false;
        }
        m_read_idx += bytes_read;
    }
    printf("读取到了数据 : %s\n", m_read_buf);
    return true;
}

// 主状态机，解析请求
http_conn::HTTP_CODE http_conn:: process_read() {

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;

    char* text = 0;

    while(((m_check_state == CHECK_STATE_CONTENT) && (line_status == LINE_OK))
       || ((line_status = parse_line()) == LINE_OK)) {
            // 解析到了一行完整的数据，或者解析到了请求体，也是一块完整的数据

            // 获取一行数据
            text = get_line();
            
            m_start_line = m_checked_index;   // 这是下一行的起始位置
            printf("got 1 http line : %s\n", text);

            switch(m_check_state) {
                case CHECK_STATE_REQUESTLINE: {
                    printf("got requestline http line : %s\n", text);
                    ret = parse_request_line(text);
                    if (ret == BAD_REQUEST) {
                        return BAD_REQUEST;
                    }
                    break;

                }

                case CHECK_STATE_HEADER: {
                    printf("got head http line : %s\n", text);
                    ret = parse_headers(text);
                    if (ret == BAD_REQUEST) {
                        return BAD_REQUEST;
                    }
                    else if (ret ==  GET_REQUEST) {
                        return do_request();
                    }
                    break;

                }

                case CHECK_STATE_CONTENT: {
                    printf("got content http line : %s\n", text);
                    ret = parse_content(text);
                    if (ret == GET_REQUEST) {
                        return do_request();
                    }
                    line_status = LINE_OPEN; // 表示数据不完成
                    break;

                }

                default: {
                    return INTERNAL_ERROR;

                }

            }
    }

    return NO_REQUEST;
}                

// 解析HTTP请求行，获得请求方法，目标URL,HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char* text) {
    //  GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t");  // 检测text中，' ' 和 '\t'哪个先出现，并将位置返回

    //   GET\0/index.html HTTP/1.1
    *m_url++ = '\0';

    char* method = text;
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    }
    else {
        return BAD_REQUEST;
    }

    //    /index.html HTTP/1.1
    m_version = strpbrk(m_url, " \t");
    if (!m_version) {
        return BAD_REQUEST;
    }

    //   /index.html\0HTTP/1.1
    *m_version++ = '\0';
    if (strcasecmp(m_version, "HTTP/1.1") != 0){
        return BAD_REQUEST;
    }

    // 因为有的是http://192.168.1.1:1000/index.html
    if (strncasecmp(m_url, "http://", 7) == 0){
        m_url += 7;     // 192.168.1.1:1000/index.html
        m_url = strchr(m_url, '/');      //  /index.html
    }

    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    m_check_state = CHECK_STATE_HEADER;    // 请求行已经检查完毕，成功，所以接下来要检测请求头部，所以主状态机的检查状态变成检查请求头

    return NO_REQUEST;
}

// 解析HTTP请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char* text) { 

    // 遇到空行，表示头部字段解析完毕
    if (text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        // 主状态机状态要转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {  
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;  // 表示还没有解析完整，还要继续解析
        }

        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;     // 这个地方返回这个，表示没有请求体，请求到这就结束了
    }
    else if ( strncasecmp(text, "Connection:", 11) == 0) {
        // 处理Connection 头部字段 Conection : keep-alive
        text += 11;
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;  // 保持连接
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        // 处理Content-Length头部字段
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    else if (strncasecmp(text, "Host:", 5) == 0) {
        // 处理Host头部字段
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else {
        printf("oop! unknow header %s\n", text);
    }

    return NO_REQUEST;

}

// 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
http_conn::HTTP_CODE http_conn::parse_content(char* text){
    
    if (m_read_idx >= ( m_content_length + m_checked_index)) {
        text[m_content_length] = '\0';  // 在这里截断，会影响到下一个请求吗，会把下一个请求的第一个字符改了吗
        return GET_REQUEST;
    }

    return NO_REQUEST;
}  

// 解析一行，判断依据\r\n
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;

    for ( ; m_checked_index < m_read_idx; ++m_checked_index){
        temp = m_read_buf[m_checked_index];

        if (temp == '\r') {
            if ((m_checked_index + 1) == m_read_idx) {     // 如果当前字符是\r，但是缓冲中，已经没有数据了，这是最后一个数据，所以表示数据不完整
                return LINE_OPEN;                          // 数据没请求完
            }
            else if (m_read_buf[m_checked_index + 1] == '\n') {       // 当前字符是\r,如果下一个字符是\n，说明读取到了一行，然后将\r\n都变成\0,
                m_read_buf[m_checked_index++] = '\0';                 // 相当于从缓冲中，截出这一行
                m_read_buf[m_checked_index++] = '\0';
                return LINE_OK;
            }

            return LINE_BAD;
        }
        else if (temp == '\n') {

            if ((m_checked_index > 1) && (m_read_buf[m_checked_index -1] == '\r')) {  // 针对上面的第一种情况，读到\r,但是缓冲区没数据了
                m_read_buf[m_checked_index-1] = '\0';                                 // 然后来了，数据，一检测，第一个字符是\n,前面一个字符又是\r,
                m_read_buf[m_checked_index++] = '\0';                                 // 说明这是一行
                return LINE_OK;                                                   
            }                                                                         
            return LINE_BAD;
        }
        // return LINE_OPEN;   警钟长鸣，我靠
    }




    return LINE_OK;
}

// 当得到一个完成、正确的HTTP请求时，我们就分析目标文件的属性
// 如果目标文件存在，对所有用户可读，且不是目录，则使用mmap将其
// 映射到内存地址m_file_address处，并告诉调用者获取文件成功
http_conn:: HTTP_CODE http_conn :: do_request(){

    //  "/home/wensong/webserver/resources"
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len -1);   // 服务器的项目根目录 + 请求文件的目录， 合体
    // 获取m_real_file文件的相关的状态信息，-1 失败， 0 成功
    if (stat(m_real_file, &m_file_stat) < 0) {
        return NO_RESOURCE;
    }
    
    // 判断访问权限
    if (!(m_file_stat.st_mode &S_IROTH)) {
        return FORBIDDEN_REQUEST;
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;
    }

    // 以只读方式打开文件
    int fd = open( m_real_file, O_RDONLY );
    // 创建内存映射
    m_file_address = ( char* )mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0 );
    close( fd );
    return FILE_REQUEST;
}


// 对内存映射区执行munmap操作, 即用完了，释放掉
void http_conn::unmap() {
    if( m_file_address )
    {
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}


// 写HTTP响应
bool http_conn::write()
{
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    // int bytes_to_send = m_write_idx;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    int bytes_to_send = m_write_idx + m_file_stat.st_size;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modfd( m_epollfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }
    /*
    雀氏是有问题的这里，用的是分散写，两块地址，但是上面的bytes_to_send赋的值是写缓冲区中的长度，不包含响应正文的长度
    但是下面用bytes_to_send - temp 去判断是否发送完毕，而temp是包含两块地方的长度的，所以有问题，
    这就是为什么，我传输一张的像素大的照片会传输失败，
    原因： 
    1. 下面判断HTTP中响应是否发送完毕的if条件中，之前用的是bytes_to_send <= bytes_have_send,这坑定是错的，bytes_to_send一直在递减，
    所以导致，如果两块地址的东西太大，需要传输多次，那必定是没传输完毕就终止了，现在改了成m_write_idx
    
    2. 改了m_write_idx，也有问题，因为m_write_idx是响应行+响应头的长度，响应正文的长度没加，现在加上
    
    3. 还有如果tcp缓冲区没有空间了，然后重新发送，但是有个问题，这里重新发送没有接着前面已经发送的后面发送，还是重新发，那会导致数据一直是不完整的
    */
    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                // modfd( m_epollfd, m_sockfd, EPOLLOUT );   // 如果在这里重置EPOLLOUT事件，并且返回的话，那等缓冲区有了空间，重新执行write，导致重新从头发送数据，那么永远也发不完整
                // return true;  // 返回true，是不关这个tcp连接，然后继续写
                continue;      // 如果是tcp写缓冲区满了，那就循环，等到缓冲区不满了，再继续写，可是这样写了之后，大文件还是会被截断，为什么
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;   // 这句感觉没什么用
        bytes_have_send += temp;
        if ( m_write_idx + m_file_stat.st_size <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linger) {    // 这里的m_linger通过下面return true还是false起作用，如果是false，外面就close(fd)了
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modfd( m_epollfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

// 往写缓冲中写入待发送的数据
bool http_conn::add_response( const char* format, ... ) {
    if( m_write_idx >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_idx ) ) {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list );
    return true;
}

bool http_conn::add_status_line( int status, const char* title ) {
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response( "Content-Length: %d\r\n", content_len );
}

bool http_conn::add_linger()
{
    return add_response( "Connection: %s\r\n", ( m_linger == true ) ? "keep-alive" : "close" );
}

bool http_conn::add_blank_line()
{
    return add_response( "%s", "\r\n" );
}

bool http_conn::add_content( const char* content )
{
    return add_response( "%s", content );
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}


// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE ret) {
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            add_headers(m_file_stat.st_size);
            m_iv[ 0 ].iov_base = m_write_buf;
            m_iv[ 0 ].iov_len = m_write_idx;
            m_iv[ 1 ].iov_base = m_file_address;
            m_iv[ 1 ].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            return true;
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

// 由线程池中的工作线程调用，这是处理http请求的入口函数
void http_conn :: process() {

    // 解析http请求
    HTTP_CODE read_ret = process_read();

    if (read_ret == NO_REQUEST) {
        modfd(m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    // 生成响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT);

}    






