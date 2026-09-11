package main

import "core:fmt"
import "core:mem"
import "core:hash/xxhash"

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
