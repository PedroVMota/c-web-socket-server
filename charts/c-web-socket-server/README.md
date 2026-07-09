# c-web-socket-server Helm Chart

This chart deploys the C TCP chat server image published by this repository:

```text
ghcr.io/pedrovmota/c-web-socket-server:latest
```

Render locally:

```sh
helm template c-web-socket-server charts/c-web-socket-server
```

Install into a cluster:

```sh
helm upgrade --install c-web-socket-server charts/c-web-socket-server --namespace dev --create-namespace
```

The application listens on TCP port `5555` and is exposed by a ClusterIP
Service. Use port-forwarding, a LoadBalancer Service, or Traefik TCP routing for
external clients.
