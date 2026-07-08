FROM alpine:3.20 AS build

RUN apk add --no-cache build-base
WORKDIR /src

COPY Makefile ./
COPY src ./src
COPY tests ./tests

RUN make test
RUN make clean && make

FROM alpine:3.20

RUN addgroup -S chat && adduser -S -G chat chat
WORKDIR /app

COPY --from=build /src/chat-server /usr/local/bin/chat-server

ENV CHAT_HOST=0.0.0.0
ENV CHAT_PORT=5555
ENV CHAT_MAX_CLIENTS=64
ENV CHAT_BACKLOG=16
ENV CHAT_BUFFER_SIZE=4096

EXPOSE 5555

USER chat
ENTRYPOINT ["/usr/local/bin/chat-server"]
