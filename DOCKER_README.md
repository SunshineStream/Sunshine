# Docker

## Important note
Starting with v0.18.0, tag names have changed. You may no longer use `latest`, `master`, `vX.X.X`.

## Build your own containers
This image provides a method for you to easily use the latest Sunshine release in your own docker projects. It is not
intended to use as a standalone container at this point, and should be considered experimental.

```dockerfile
ARG SUNSHINE_VERSION=latest
ARG SUNSHINE_OS=ubuntu-22.04
FROM lizardbyte/sunshine:${SUNSHINE_VERSION}-${SUNSHINE_OS}

# install Steam, Wayland, etc.

ENTRYPOINT steam && sunshine
```

### SUNSHINE_VERSION
- `latest`, `master`, `vX.X.X`
- commit hash

### SUNSHINE_OS
Sunshine images are available with the following tag suffixes, based on their respective base images.

- `debian-bookworm`
- `ubuntu-22.04`
- `ubuntu-24.04`

### Tags
You must combine the `SUNSHINE_VERSION` and `SUNSHINE_OS` to determine the tag to pull. The format should be
`<SUNSHINE_VERSION>-<SUNSHINE_OS>`. For example, `latest-ubuntu-24.04`.

See all our available tags on [docker hub](https://hub.docker.com/r/lizardbyte/sunshine/tags) or
[ghcr](https://github.com/orgs/LizardByte/packages/container/package/sunshine) for more info.

## Where used
This is a list of docker projects using Sunshine. Something missing? Let us know about it!

- [Games on Whales](https://games-on-whales.github.io)

## Port and Volume mappings
Examples are below of the required mappings. The configuration file will be saved to `/config` in the container.

### Using docker run
Create and run the container (substitute your `<values>`):

```bash
docker run -d \
  --device /dev/dri/ \
  --name=<image_name> \
  --restart=unless-stopped \
  --ipc=host \
  -e PUID=<uid> \
  -e PGID=<gid> \
  -e TZ=<timezone> \
  -v <path to data>:/config \
  -p 47984-47990:47984-47990/tcp \
  -p 48010:48010 \
  -p 47998-48000:47998-48000/udp \
  <image>
```

### Using docker-compose
Create a `docker-compose.yml` file with the following contents (substitute your `<values>`):

```yaml
version: '3'
services:
  <image_name>:
    image: <image>
    container_name: sunshine
    restart: unless-stopped
    volumes:
      - <path to data>:/config
    environment:
      - PUID=<uid>
      - PGID=<gid>
      - TZ=<timezone>
    ipc: host
    ports:
      - "47984-47990:47984-47990/tcp"
      - "48010:48010"
      - "47998-48000:47998-48000/udp"
```

### Using podman run
Create and run the container (substitute your `<values>`):

```bash
podman run -d \
  --device /dev/dri/ \
  --name=<image_name> \
  --restart=unless-stopped \
  --userns=keep-id \
  -e PUID=<uid> \
  -e PGID=<gid> \
  -e TZ=<timezone> \
  -v <path to data>:/config \
  -p 47984-47990:47984-47990/tcp \
  -p 48010:48010 \
  -p 47998-48000:47998-48000/udp \
  <image>
```

### Parameters
You must substitute the `<values>` with your own settings.

Parameters are split into two halves separated by a colon. The left side represents the host and the right side the
container.

**Example:** `-p external:internal` - This shows the port mapping from internal to external of the container.
Therefore `-p 47990:47990` would expose port `47990` from inside the container to be accessible from the host's IP on
port `47990` (e.g. `http://<host_ip>:47990`). The internal port must be `47990`, but the external port may be changed
(e.g. `-p 8080:47990`). All the ports listed in the `docker run` and `docker-compose` examples are required.


| Parameter                   | Function             | Example Value      | Required |
|-----------------------------|----------------------|--------------------|----------|
| `-p <port>:47990`           | Web UI Port          | `47990`            | True     |
| `-v <path to data>:/config` | Volume mapping       | `/home/sunshine`   | True     |
| `-e PUID=<uid>`             | User ID              | `1001`             | False    |
| `-e PGID=<gid>`             | Group ID             | `1001`             | False    |
| `-e TZ=<timezone>`          | Lookup [TZ value][1] | `America/New_York` | False    |

For additional configuration, it is recommended to reference the *Games on Whales*
[sunshine config](https://github.com/games-on-whales/gow/blob/2e442292d79b9d996f886b8a03d22b6eb6bddf7b/compose/streamers/sunshine.yml).

[1]: https://en.wikipedia.org/wiki/List_of_tz_database_time_zones

#### User / Group Identifiers:
When using data volumes (-v flags) permissions issues can arise between the host OS and the container. To avoid this
issue you can specify the user PUID and group PGID. Ensure the data volume directory on the host is owned by the same
user you specify.

In this instance `PUID=1001` and `PGID=1001`. To find yours use id user as below:

```bash
$ id dockeruser
uid=1001(dockeruser) gid=1001(dockergroup) groups=1001(dockergroup)
```

If you want to change the PUID or PGID after the image has been built, it will require rebuilding the image.

## Supported Architectures

Specifying `lizardbyte/sunshine:latest-<SUNSHINE_OS>` or `ghcr.io/lizardbyte/sunshine:latest-<SUNSHINE_OS>` should
retrieve the correct image for your architecture.

The architectures supported by these images are shown in the table below.

| tag suffix      | amd64/x86_64 | arm64/aarch64 |
|-----------------|--------------|---------------|
| debian-bookworm | ✅            | ✅             |
| ubuntu-22.04    | ✅            | ✅             |
| ubuntu-24.04    | ✅            | ✅             |
