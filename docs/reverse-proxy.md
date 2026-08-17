# Reverse Proxy Setup

PengyCPP Web is designed for single-user personal use. It has no built-in authentication. For remote access, put it behind a reverse proxy with SSL.

## Quick start (nginx)

```nginx
server {
    listen 443 ssl;
    server_name pengy.example;

    ssl_certificate     /etc/letsencrypt/live/pengy.example/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/pengy.example/privkey.pem;

    location / {
        proxy_pass http://127.0.0.1:5000;
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $remote_addr;
    }
}
```

Then run PengyCPP Web bound to loopback with `--trusted-host`:

```bash
pengy-web --trusted-host pengy.example
```

## What does `--trusted-host` do?

PengyCPP Web rejects requests whose `Origin` doesn't match the host they were sent to (CSRF protection), and — when bound to loopback — rejects unexpected `Host` headers (DNS rebinding protection). When a reverse proxy forwards requests, the `Host` header is the public domain, not `localhost`. `--trusted-host` tells PengyCPP to accept that.

## When you need it

You need `--trusted-host` if **all** of these are true:

- PengyCPP is bound to loopback (the default — no `--host` flag)
- Something proxies to it (nginx, Caddy, Traefik, `ssh -L`)
- You reach it under a hostname other than `localhost`

You do **not** need it when PengyCPP binds the network itself (`--host 0.0.0.0`) — including Docker containers with a published port, or LAN access from other devices. Those work unmodified.

## Symptom of a missing flag

Every request returns 403, or GETs work while every POST 403s. Add `--trusted-host` with the public hostname to fix it.

## Other proxy examples

### Caddy

```
pengy.example {
    reverse_proxy 127.0.0.1:5000
}
```

No `--trusted-host` needed — Caddy sets the original `Host` header by default.

### SSH tunnel

```bash
ssh -L 5000:localhost:5000 user@server
# Then open http://localhost:5000 locally — works without --trusted-host
```

### Docker

```bash
docker run -p 5000:5000 pengycpp
# Then open http://server-ip:5000 — works without --trusted-host
# (PengyCPP sees the container's IP as the bind address, not loopback)
```
