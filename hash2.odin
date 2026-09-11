package main

import "core:fmt"
import "core:hash/xxhash"
//Streaming hashing (Composite Keys or WAL records)
main :: proc() {
    // Note: XXH3_state has an explicit #align(64) requirement!
    state: xxhash.XXH3_state
    xxhash.XXH3_64_reset(&state)

    tenant_id: u32 = 42
    user_id:   u64 = 100921

    // Hash first column
    tenant_bytes := transmute([4]u8)tenant_id
    xxhash.XXH3_64_update(&state, tenant_bytes[:])

    // Hash second column into the same state
    user_bytes := transmute([8]u8)user_id
    xxhash.XXH3_64_update(&state, user_bytes[:])

    // Finalize digest
    composite_hash := xxhash.XXH3_64_digest(&state)
    fmt.printfln("Composite Hash: 0x%16x", composite_hash)
}
