#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;//用户名和密码
//将数据库中的用户名和密码载入到服务器的map中来，map中的key为用户名，value为密码
void http_conn::initmysql_result(connection_pool *connPool)
{
    //先从数据库连接池中取一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在user表中检索username，passwd数据，浏览器端输入
    if (mysql_query(mysql, "SELECT username,passwd FROM user"))
    {
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数组
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while (MYSQL_ROW row = mysql_fetch_row(result))
    {
        string temp1(row[0]);
        string temp2(row[1]);
        users[temp1] = temp2;
    }
}

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    //fcntl提供对文件描述符的各种控制操作
    int old_option = fcntl(fd, F_GETFL);//获取fd的状态标志
    int new_option = old_option | O_NONBLOCK;//在fd原有的状态下设置非阻塞
    fcntl(fd, F_SETFL, new_option);//将新的非阻塞的状态标志写入fd
    return old_option;//返回旧的状态
}

//将内核事件表注册读事件，ET/LT模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;//ET模式
    else
        event.events = EPOLLIN | EPOLLRDHUP;//LT模式

    if (one_shot)
        event.events |= EPOLLONESHOT;//oneshot模式
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);//往内核事件表中注册fd上的事件
    setnonblocking(fd);
}

//从内核事件表删除描述符 内核事件表删除事件
void removefd(int epollfd, int fd)
{
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);//从内核事件表删除fd事件
    close(fd);
}

//将事件重置为EPOLLONESHOT 
//防止当目前的线程处理完这个socket中的数据后 下一次这个socket再可读时 其他的线程不能够触发这个socket上的可读事件
//因此当前线程处理完socket上的读写后 需要重置EPOLLONESHOT状态
void modfd(int epollfd, int fd, int ev, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;

    if (1 == TRIGMode)
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;//ET边缘触发
    else
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);//修改fd事件
}
//初始连接数为0
int http_conn::m_user_count = 0;
//初始化epoll内核事件表
int http_conn::m_epollfd = -1;

//关闭连接，关闭一个连接，客户总量减一
void http_conn::close_conn(bool real_close)
{
    if (real_close && (m_sockfd != -1))
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode,
                     int close_log, string user, string passwd, string sqlname)
{
    m_sockfd = sockfd;
    m_address = addr;
    //内核事件表注册事件
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//从状态机，用于分析出一行内容
//返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
//m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
//m_checked_idx指向从状态机当前正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for (; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buf[m_checked_idx];//temp为要分析的子节
        if (temp == '\r')//如果当前是\r字符 
        {
            if ((m_checked_idx + 1) == m_read_idx)//下一个字符到达了buffer结尾 则接受不完整 需要继续接收
                return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')//下一个字符是\n，将\r\n改为\0\0
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;//接收完整
            }
            return LINE_BAD;//如果都不符合 则返回语法错误
        }
        //如果当前字符是\n，也有可能读取到完整行 一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if (temp == '\n')
        {
            //如果前一个字符是\r，则将\r\n修改成\0\0，将m_checked_idx指向下一行的开头，则返回LINE_OK
            if (m_checked_idx > 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //当前字节既不是\r，也不是\n 表示接收不完整，需要继续接收，返回LINE_OPEN
    return LINE_OPEN;
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
//read_once读取浏览器端发送来的请求报文，直到无数据可读或对方关闭连接，读取到m_read_buffer中，并更新m_read_idx
bool http_conn::read_once()
{
    if (m_read_idx >= READ_BUFFER_SIZE)//缓冲区中数据的最后一个子节下一个位置已经大于缓冲区大小了 不能继续写入
    {
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if (0 == m_TRIGMode)
    {
        //从套接字接收数据 存储在m_read_buf缓冲区 第二个参数为缓冲区位置 第三个参数为缓冲区大小 最后一个参数为额外的设置 一般为0
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            //从套接字接收数据 存储在m_read_buf缓冲区
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                //非阻塞ET模式下 需要一次性将数据读完
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                return false;
            }
            else if (bytes_read == 0)
            {
                return false;
            }
            m_read_idx += bytes_read;//修改m_read_idx的读取字节数
        }
        return true;
    }
}

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text)
{
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    m_url = strpbrk(text, " \t");
    if (!m_url)//如果没有空格或者\t 则报文格式有误
    {
        return BAD_REQUEST;
    }
    *m_url++ = '\0';//将该位置改为\0，用于将前面数据取出
    char *method = text;//取出数据，并通过与GET和POST比较，以确定请求方式
    if (strcasecmp(method, "GET") == 0)
        m_method = GET;
    else if (strcasecmp(method, "POST") == 0)
    {
        m_method = POST;
        cgi = 1;
    }
    else
        return BAD_REQUEST;//不符合这两种格式 就是报文格式有误
    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");//使用与判断请求方式的相同逻辑，判断HTTP版本号
    if (!m_version)
        return BAD_REQUEST;//报文格式有误
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    //同样增加https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }
    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if (!m_url || m_url[0] != '/')
        return BAD_REQUEST;
    //当url为/时，显示判断界面
    if (strlen(m_url) == 1)
        strcat(m_url, "judge.html");
    m_check_state = CHECK_STATE_HEADER;//请求行处理完毕，将主状态机转移处理请求头
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    //判断是空行还是请求头 
    if (text[0] == '\0')//若是空行
    {
        if (m_content_length != 0)//不是0 表明是POST请求 则状态转移到 CHECK_STATE_CONTENT
        {
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则是GET请求 则报文解析结束
        return GET_REQUEST;
    }
    //解析请求头部连接字段  若解析的是请求头部字段，则主要分析connection字段，content-length字段
    else if (strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        //跳过空格和\t字符
        text += strspn(text, " \t");
        if (strcasecmp(text, "keep-alive") == 0)//如果是长连接，则将linger标志设置为true
        {
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//解析POST消息体
//判断http请求是否被完整读入
//仅用于解析POST请求 调用parse_content函数解析消息体 
//用于保存post请求消息体 为后面的登陆和注册做准备
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text; //将消息体写入m_string中
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::process_read()
{
    LINE_STATUS line_status = LINE_OK;//初始化从状态机的状态
    HTTP_CODE ret = NO_REQUEST;//初始化HTTP请求解析结果
    char *text = 0;
    //判断条件 主状态机转移到CHECK_STATE_CONTENT，该条件涉及解析消息体 
    //从状态机转移到LINE_OK，该条件涉及解析请求行和请求头部
    //两者为或关系，当条件为真则继续循环，否则退出
    //循环体：
    //从状态机读取数据 调用get_line函数，通过m_start_line将从状态机读取数据间接赋给text 主状态机解析text
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line();//调用get_line函数
        //m_start_line是每一个数据行在m_read_buf中的起始位置
        //m_checked_idx表示从状态机在m_read_buf中读取的位置
        m_start_line = m_checked_idx;//通过m_start_line将从状态机读取数据间接赋给text
        LOG_INFO("%s", text);
        //主状态机的三种状态转移逻辑
        switch (m_check_state)
        {
        case CHECK_STATE_REQUESTLINE://解析请求行
        {
            ret = parse_request_line(text);//解析请求行
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            break;
        }
        case CHECK_STATE_HEADER:
        {
            ret = parse_headers(text);//解析请求头部
            if (ret == BAD_REQUEST)
                return BAD_REQUEST;
            else if (ret == GET_REQUEST)//完整解析GET请求后 跳转到报文响应函数
            {
                return do_request();
            }
            break;
        }
        case CHECK_STATE_CONTENT:
        {
            ret = parse_content(text);//解析消息体
            if (ret == GET_REQUEST)
                return do_request();//完整解析POST请求后 跳转到报文响应函数
            line_status = LINE_OPEN;//解析完消息体即完成报文解析 避免再次进入循环 更新line_status
            break;
        }
        default:
            return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}
//该函数将网站根目录和url文件拼接，然后通过stat判断该文件属性。另外，为了提高访问速度，通过mmap进行映射，将普通文件映射到内存逻辑地址
http_conn::HTTP_CODE http_conn::do_request()
{
    //将初始化的m_real_file赋值为网站根目录 
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);//长度
    //printf("m_url:%s\n", m_url);
    const char *p = strrchr(m_url, '/');//找到m_url中/的位置 

    //处理cgi
    //实现登陆和注册校验
    if (cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {

        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';
        //以&为分隔符，后面的是密码
        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");
            //判断map中能否找到重复的用户名
            if (users.find(name) == users.end())
            {
                //向数据库中插入数据时 需要通过锁来同步数据
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                users.insert(pair<string, string>(name, password));
                m_lock.unlock();
                //校验成功 跳转至登陆界面
                if (!res)
                    strcpy(m_url, "/log.html");
                //校验失败 跳转至注册失败页面
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html");
            else
                strcpy(m_url, "/logError.html");
        }
    }
    //如果请求资源为/0 表示跳转到注册界面
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1 表示跳转到登陆界面 
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')//图片界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')//视频界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')//关注界面
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else//否则发url实际请求的文件
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    //通过stat获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if (stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;
    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if (!(m_file_stat.st_mode & S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if (S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;
    //以只读方式获取客户端申请的资源的文件描述符，通过mmap将该文件映射到内存中(即共享内存)
    int fd = open(m_real_file, O_RDONLY);
    //mmap成功时返回共享的内存空间的指针
    //参数 申请的内存空间的起始地址 资源的实际大小作为申请的共享内存的大小 这段内存可读 内存段为调用的进程私有(不在所有进程之间共享)
    //fd为客户端申请资源的文件描述符 最后一个参数表示从文件的何处进行映射 设置为0表示映射整个文件到申请的共享内存
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);//避免文件描述符的浪费和占用 
    return FILE_REQUEST;//表示请求文件存在 且可以访问
}
void http_conn::unmap()//将一个文件或其它对象映射到内存 提高文件的访问速度
{
    if (m_file_address)
    {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}
bool http_conn::write()
{
    int temp = 0;

    //若要发送的数据长度为0 表示响应报文为空 一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//在epoll树上重置EPOLLONESHOT事件
        init();
        return true;
    }

    while (1)
    {
        //将响应报文的状态行、消息头、空行和响应正文发送给浏览器端
        //writev将多块分散的内存块内的数据一并写入到发送缓存中 因为相应的文件为mmap单独存储在一块内存空间
        //与状态行 头部不在一起 因此需要用writev聚集写
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //未正常发送，temp为发送的字节数
        if (temp < 0)
        {
            if (errno == EAGAIN)//若是eagain 则是写缓冲区满了
            {
                //重新注册写事件 等待下一次写事件触发
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            //如果发送失败 但不是缓冲区问题 则取消文件映射
            unmap();
            return false;
        }
        //正常发送 temp为发送的子节
        bytes_have_send += temp;//更新已经发送的字节
        bytes_to_send -= temp;//更新待发送的字节
        //第一个iovec头部信息的数据已发送完，发送第二个iovec数据
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        else//继续发送第一个iovec头部信息的数据
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        //判断条件，数据已全部发送完
        if (bytes_to_send <= 0)
        {
            unmap();//取消映射
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//在epoll树上重新注册EPOLLONESHOT事件

            if (m_linger)//浏览器的请求为长连接
            {
                init();//重新初始化HTTP对象
                return true;
            }
            else
            {
                return false;
            }
        }
    }
}
//内部调用add_response函数更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char *format, ...)
{
    //如果写入内容超出m_write_buf大小则报错 
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    va_list arg_list;//定义可变参数列表
    va_start(arg_list, format);//将变量arg_list初始化为传入参数
    //将数据format从可变参数列表写入缓冲区写，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    m_write_idx += len;//更新m_write_idx位置
    va_end(arg_list);//清空可变参列表

    LOG_INFO("request:%s", m_write_buf);

    return true;
}
bool http_conn::add_status_line(int status, const char *title)//添加状态行
{
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
bool http_conn::add_headers(int content_len)//添加消息报头，具体的添加文本长度、连接状态和空行
{
    return add_content_length(content_len) && add_linger() &&
           add_blank_line();
}
bool http_conn::add_content_length(int content_len)//添加Content-Length，表示响应报文的长度
{
    return add_response("Content-Length:%d\r\n", content_len);
}
bool http_conn::add_content_type()//添加文本类型，这里是html
{
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool http_conn::add_linger()//添加连接状态，通知浏览器端是保持连接还是关闭
{
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
bool http_conn::add_blank_line()//添加空行
{
    return add_response("%s", "\r\n");
}
bool http_conn::add_content(const char *content)//添加文本content
{
    return add_response("%s", content);
}
//根据do_request的返回状态，服务器子线程调用process_write向m_write_buf中写入响应报文
bool http_conn::process_write(HTTP_CODE ret)
{
    switch (ret)
    {
    case INTERNAL_ERROR://服务器内部错误
    {
        add_status_line(500, error_500_title);//添加状态行 5xx：服务器端错误--服务器处理请求出错
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST://HTTP请求报文有语法错误或请求资源为目录
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST://请求资源禁止访问，没有读取权限
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST://请求资源可以正常访问
    {
        add_status_line(200, ok_200_title);//添加状态行
        if (m_file_stat.st_size != 0)//如果请求的资源在
        {
            add_headers(m_file_stat.st_size);//添加消息头
            m_iv[0].iov_base = m_write_buf;//第一个iovec指针指向响应报文缓冲区，长度指向m_write_idx
            m_iv[0].iov_len = m_write_idx;
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;//发送的全部数据为响应报文头部信息和文件的大小
            return true;
        }
        else
        {
            //如果请求的文件大小为0 则返回空白html文件
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //除FILE_REQUEST状态外，其余状态只申请一个iovec，指向响应报文缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}
//浏览器端发出http连接请求 服务器端主线程创建http对象接收请求 并将所有数据读入buffer 将该对象插入任务队列后
//工作线程从任务队列中取出一个任务进行处理
//各子线程通过process函数对任务进行处理 调用process_read函数和process_write函数分别完成对报文解析与响应
//两个任务
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST)//NO_REQUEST表示请求不完整 需要继续接收请求数据
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);//注册并监听读事件
        return;
    }
    bool write_ret = process_write(read_ret);//调用process_write完成报文响应
    if (!write_ret)
    {
        close_conn();
    }
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);//注册并监听写事件
}
