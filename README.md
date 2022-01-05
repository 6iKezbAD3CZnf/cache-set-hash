# cache-set-hash

## How to use
```
# Install gem5
git clone -b v21.1.0.2 https://gem5.googlesource.com/public/gem5 ./gem5

# Run docker (optional)
docker pull gcr.io/gem5-test/ubuntu-20.04_all-dependencies:v21-2
docker run -u $UID:$GID --volume <cache-set-hash directory>:/csh --rm -it gcr.io/gem5-test/ubuntu-20.04_all-dependencies:v21-2
(docker) cd csh

# Hard link
make hl

# Build gem5
make build

# Run hello
make csh
```
