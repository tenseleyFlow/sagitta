FROM scratch
WORKDIR /opt/yew
ADD build/yew /usr/bin/yew
USER 65532:65532
ENTRYPOINT ["/usr/bin/yew"]
