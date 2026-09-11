package main

import "core:fmt"
import "core:mem"
import "core:hash/xxhash"
//Packed Struct + One Shot
//Use Packed Struct + One Shot when all key columsn are fixed-width(integers, UUIDs, Dates)
// 1. Pack the composite key struct so there is no padding between fields
Composite_Key :: struct #packed {
    tenant_id: u32,
    user_id:   u64,
}

hash_composite_key :: proc(key: ^Composite_Key, seed: u64 = 0) -> u64 {
    // Treat the struct as a raw byte slice
    bytes := mem.byte_slice(key, size_of(Composite_Key))
    return xxhash.XXH64(bytes, seed)
}

main :: proc() {
    key := Composite_Key{
        tenant_id = 42,
        user_id   = 100921,
    }

    h := hash_composite_key(&key)
    fmt.printfln("One-shot Composite XXH64: 0x%016x", h)
}
//Seed chaining. when columns can not be contiguous. If columns are stored separately or are dynamic(string + u64), we can chain
//one shot calls by passing the previous conlumn's hash as the seed of the next
seed: u64 = 0xdeadbeef_cafebabe

// Step 1: Hash first column
tenant_bytes := transmute([4]u8)tenant_id
h1 := xxhash.XXH64(tenant_bytes[:], seed)

// Step 2: Use h1 as the seed for the second column
user_bytes := transmute([8]u8)user_id
composite_hash := xxhash.XXH64(user_bytes[:], h1)
