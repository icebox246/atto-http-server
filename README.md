# Atto HTTP server

Very simple HTTP server written in C. 
It supports only GET requests.
Currently only POSIX systems are supported.

## Quick start

```console
./build.sh

./attos -p 8080 .
```

## Features

* GET requests for served files
* Basic content type recognition based on file extension, supports:
    - `.html` -> `text/html`
    - `.css` -> `text/css`
    - `.js` -> `text/javascript`
    - `.png` -> `image/png`
    - `.jpg` -> `image/jpg`
    - `.bmp` -> `image/bmp`
* Listing files in directories
