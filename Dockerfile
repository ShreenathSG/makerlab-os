FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends build-essential libsqlite3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
COPY tests/core_test.c ./tests/core_test.c
RUN make all build/core_test && ./build/core_test

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libsqlite3-0 curl && rm -rf /var/lib/apt/lists/* \
    && useradd --uid 10001 --create-home makerlab && mkdir -p /app/data && chown makerlab:makerlab /app/data
WORKDIR /app
COPY --from=build /src/build/makerlab ./makerlab
COPY static ./static
USER makerlab
ENV MAKERLAB_DB=/app/data/makerlab-c.db
EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=3s --start-period=5s CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1
CMD ["./makerlab", "--host", "0.0.0.0"]
