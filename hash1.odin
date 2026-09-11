package main
//One shot hashing (Standard for Hash Joins)
import "core:fmt"
import "core:hash/xxhash"

main :: proc() {
    key := transmute([]u8)"customer_id_98741"

    // 1. Default (unseeded) XXH3 64-bit
    h1 := xxhash.XXH3_64(key)
    fmt.printfln("XXH3_64:        0x%16x", h1)

    // 2. Seeded XXH3 64-bit (useful to prevent hash-flooding DoS)
    seed: u64 = 0xdeadbeef_cafebabe
    h2 := xxhash.XXH3_64(key, seed)
    fmt.printfln("XXH3_64 Seeded: 0x%16x", h2)

    // 3. XXH3 128-bit
    h128 := xxhash.XXH3_128(key)
    fmt.printfln("XXH3_128:       0x%32x", h128)
}
