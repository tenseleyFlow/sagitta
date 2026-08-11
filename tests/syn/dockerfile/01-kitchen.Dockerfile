# syntax=docker/dockerfile:1
FROM --platform=linux/amd64 alpine:3.20
ARG VERSION=1
ENV APP_HOME=/srv/app
LABEL org.opencontainers.image.title="yew"
COPY --chown=1000:1000 ["src", "/app"]
RUN echo "$APP_HOME" && printf 'ok' # shell stays code
CMD ["yew", "--version"]
