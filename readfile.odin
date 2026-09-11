package main

import "core:fmt"
import "core:os"
import "core:hash/xxhash"

hash_file_streaming :: proc(filepath: string) -> (digest: u64, ok: bool) {
    // 1. Open the file
    handle, err := os.open(filepath, os.O_RDONLY)
    if err != 0 {
        return 0, false
    }
    defer os.close(handle)

    // 2. Initialize xxHash state
    state: xxhash.XXH64_state
    xxhash.XXH64_reset_state(&state, 0)

    // 3. Read in 4 KB chunks and stream them into the hasher
    buf: [4096]u8
    for {
        bytes_read, read_err := os.read(handle, buf[:])
        if bytes_read <= 0 || read_err != 0 {
            break // Reached EOF or read error
        }

        // Only hash the slice of bytes actually read
        xxhash.XXH64_update(&state, buf[:bytes_read])
    }

    // 4. Produce the final 64-bit hash
    return xxhash.XXH64_digest(&state), true
}

main :: proc() {
    file_path := "large_data.bin"
    if hash, ok := hash_file_streaming(file_path); ok {
        fmt.printfln("File Hash: 0x%016x", hash)
    } else {
        fmt.eprintfln("Failed to read file: %s", file_path)
    }
}
