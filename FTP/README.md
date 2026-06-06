
# 环境配置

## 开发环境

推荐系统：

```text
Ubuntu 20.04+
CentOS 7+
Debian 11+
```

编译器：

```text
g++ >= 9
```

查看版本：

```bash
g++ --version
```

---

## 安装编译环境

Ubuntu：

```bash
sudo apt update

sudo apt install build-essential -y
```

---

## 项目获取

```bash
git clone <your-repository-url>

cd ftp_project
```

或者直接复制源码：

```bash
ftp_server.cpp
ftp_client.cpp
```

到对应目录。

---

## 编译项目

### 编译服务器

```bash
g++ FTP/server/ftp_server.cpp -o ftp_server -pthread
```

### 编译客户端

```bash
g++ FTP/client/ftp_client.cpp -o ftp_client
```

编译完成后：

```text
ftp_server
ftp_client
```

两个可执行文件将生成在当前目录。

---

# 项目启动流程

## 第一步：启动服务器

执行：

```bash
./ftp_server
```

启动成功显示：

```text
FTP Server started on port 2100 with epoll...
```

服务器监听：

```text
127.0.0.1:2100
```

---

## 第二步：启动客户端

新开终端：

```bash
./ftp_client
```

连接成功显示：

```text
220 ready
```

进入菜单：

```text
===== FTP Client =====

1. login
2. ls
3. cd
4. pwd
5. download
6. upload
7. quit
```

---

## 第三步：登录

选择：

```text
1
```

输入：

```text
username: hgc-qwe
password: 1234567
```

成功：

```text
230 User logged in.
```

---

## 第四步：查看目录

选择：

```text
2
```

输入：

```text
.
```

服务器返回当前目录文件列表。

---

## 第五步：下载文件

选择：

```text
5
```

示例：

```text
remote file:
/tmp/test.txt

save dir:
/home/user/download
```

下载成功：

```text
226 Transfer complete.
saved to:
/home/user/download/test.txt
```

---

## 第六步：上传文件

选择：

```text
6
```

示例：

```text
local file:
/home/user/test.txt
```

上传成功：

```text
226 Transfer complete.
```

文件保存位置：

```text
ftp_storage/test.txt
```

---

## 第七步：退出

选择：

```text
7
```

返回：

```text
221 Goodbye.
```

客户端断开连接。
