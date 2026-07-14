# syntax=docker/dockerfile:1.7
FROM --platform=$BUILDPLATFORM golang:1.26.5-alpine AS build

ARG TARGETOS
ARG TARGETARCH
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=$TARGETOS GOARCH=$TARGETARCH \
    go build -trimpath -ldflags="-s -w" -o /out/aegisub-together-server ./cmd/aegisub-together-server
RUN mkdir -p /out/data && chown 65532:65532 /out/data

FROM gcr.io/distroless/static-debian12:nonroot
WORKDIR /data
COPY --from=build --chown=65532:65532 /out/data /data
COPY --from=build /out/aegisub-together-server /usr/local/bin/aegisub-together-server
USER nonroot:nonroot
EXPOSE 8080
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/aegisub-together-server"]
CMD ["serve", "-listen", ":8080", "-database", "/data/collab.db"]
