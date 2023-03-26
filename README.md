# Proxy Server(SOCKS 4A) & Firewall

It is an implementation of SOCKS 4A protocal in the application layer of the OSI model. Similar to a proxy that acts as both server and client.
There are two types of SOCKS operations, CONNECT and BIND.


##  Usage

To make the sample program:
```bash
make
```

Run the proxy_server on 140.113.194.210 and port 5000
```bash
bash$ ./proxy_server 5000
```

**CONNECT:**

Assume your proxy_server is running on 140.113.194.210 and listening at port 5000.

Setup: Open a browser (firefox is recommend), go to setting -> network setting -> choose manual proxy configuration
-> SOCKS Host 140.113.194.210, Port 5000, choose SOCKS v4

Then you are able to connect any webpages on Google Search.

**BIND:**
Connect to FTP server with SOCKS.

Data connection mode is Active Mode (PORT)

**Firewall**
Permit destination IPs are listed into socks.conf(c for CONNECT, b for BIND)
```bash
permit c 140.113.*.* # permit NYCU IP for Connect operation
```
```bash
permit b *.*.*.* # permit all IP for Bind operation
```
