---
title: "安装部署"
sidebar_label: "安装部署"
---

## 安装部署
### 环境准备
ANode 要求节点上准备有 Python 3.10 及以上版本，以及相应的 Python 包自动安装组件 Pip，同时请确保能够正常连接互联网。

### 安装及卸载
使用专门的 ANode 安装包 TDengine-enterprise-anode-1.x.x.tar.gz 进行 ANode 的安装部署工作，安装过程与 TDengine 的安装流程一致。

```bash
tar -xzvf TDengine-enterprise-anode-1.0.0.tar.gz
cd TDengine-enterprise-anode-1.0.0
sudo ./install.sh
```

卸载 ANode，执行命令 `rmtaosanode` 即可。

### 其他
为了避免 ANode 安装后影响目标节点现有的 Python 库。 ANode 使用 Python 虚拟环境运行，安装后的默认 Python 目录处于 `/var/lib/taos/taosanode/venv/`。为了避免反复安装虚拟环境带来的开销，卸载 ANode 并不会自动删除该虚拟环境，如果您确认不需要 Python 的虚拟环境，可以手动删除。

## 启动及停止服务
安装 ANode 以后，可以使用 `systemctl` 来管理 ANode 的服务。使用如下命令可以启动/停止/检查状态。

```bash
systemctl start  taosanoded
systemctl stop   taosanoded
systemctl status taosanoded
```

## 目录及配置说明
|目录/文件|说明|
|---------------|------|
|/usr/local/taos/taosanode/bin|可执行文件目录|
|/usr/local/taos/taosanode/resource|资源文件目录，链接到文件夹 /var/lib/taos/taosanode/resource/|
|/usr/local/taos/taosanode/lib|库文件目录|
|/var/lib/taos/taosanode/model/|模型文件目录，链接到文件夹 /var/lib/taos/taosanode/model|
|/var/log/taos/taosanode/|日志文件目录|
|/etc/taos/taosanode.ini|配置文件|

### 配置说明

Anode 提供的 RestFul 服务使用 uWSGI 驱动，因此 ANode 和 uWSGI 的配置信息存放在同一个配置文件中，具体如下：

```ini
[uwsgi]
# charset
env = LC_ALL = en_US.UTF-8

# ip:port
http = 127.0.0.1:6050

# the local unix socket file than communicate to Nginx
#socket = 127.0.0.1:8001
#socket-timeout = 10

# base directory
chdir = /usr/local/taos/taosanode/lib

# initialize python file
wsgi-file = /usr/local/taos/taosanode/lib/taos/app.py

# call module of uWSGI
callable = app

# auto remove unix Socket and pid file when stopping
vacuum = true

# socket exec model
#chmod-socket = 664

# uWSGI pid
uid = root

# uWSGI gid
gid = root

# main process
master = true

# the number of worker processes
processes = 2

# pid file
pidfile = /usr/local/taos/taosanode/taosanode.pid

# enable threads
enable-threads = true

# the number of threads for each process
threads = 4

# memory useage report
memory-report = true

# smooth restart
reload-mercy = 10

# conflict with systemctl, so do NOT uncomment this
# daemonize = /var/log/taos/taosanode/taosanode.log

# log directory
logto = /var/log/taos/taosanode/taosanode.log

# wWSGI monitor port
stats = 127.0.0.1:8387

# python virtual environment directory
virtualenv = /usr/local/taos/taosanode/venv/

[taosanode]
# default app log file
app-log = /var/log/taos/taosanode/taosanode.app.log

# model storage directory
model-dir = /usr/local/taos/taosanode/model/

# default log level
log-level = DEBUG

# draw the query results
draw-result = 0
```

**提示**
请勿设置 `daemonize` 参数，该参数会导致 uWSGI 与 systemctl 冲突，从而无法正常启动。



## ANode 基本操作
#### 创建 ANode
```sql 
CREATE ANODE {node_url}
```
node_url 是提供服务的 ANode 的 IP 和 PORT, 例如：`create anode 'http://localhost:6050'`。启动 ANode 以后如果不注册到 TDengine 集群中，则无法提供正常的服务。不建议 ANode 注册到两个或多个集群中。

#### 查看 ANode
列出集群中所有的数据分析节点，包括其 `FQDN`, `PORT`, `STATUS`。
```sql
SHOW ANODES;
```

#### 查看提供的时序数据分析服务

```SQL
SHOW ANODES FULL;
```

#### 强制刷新集群中的分析算法缓存
```SQL
UPDATE ANODE {node_id}
UPDATE ALL ANODES
```

#### 删除 ANode
```sql
DROP ANODE {anode_id}
```
删除 ANode 只是将 ANode 从 TDengine 集群中删除，管理 ANode 的启停仍然需要使用`systemctl`命令。
