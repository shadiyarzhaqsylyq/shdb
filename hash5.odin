package main

import "core:fmt"
import "core:mem"
import "core:hash/xxhash"
//Packed Struct + Seed Chaining
//Use Packed Struct + Seed Chaining when composite key has both fixed-width columns and variable-length data(like VARCHAR/string)
//because they can be different in sizes('finance','IT' and etc.).
// 1. PACKED STRUCT: Holds all fixed-width key columns.
// The `#packed` directive ensures 0 alignment padding bytes.
Fixed_Key_Part :: struct #packed {
    tenant_id: u32,  // 4 bytes
    user_id:   u64,  // 8 bytes
} // Total: exactly 12 contiguous bytes

// 2. COMBINED HASH FUNCTION: Packed Struct + Seed Chaining
hash_composite_join_key :: proc(
    fixed_cols: ^Fixed_Key_Part, 
    dept_code:  string, 
    query_seed: u64 = 0xdeadbeef_cafebabe,
) -> u64 {
    // Step A: Hash all fixed columns in one shot using the query seed
    fixed_bytes := mem.byte_slice(fixed_cols, size_of(Fixed_Key_Part))
    h_fixed := xxhash.XXH64(fixed_bytes, query_seed)

    // Step B: Seed-chain into the variable-length string.
    // We pass `h_fixed` as the SEED for the string hash!
    dept_bytes := transmute([]u8)dept_code
    final_hash := xxhash.XXH64(dept_bytes, seed = h_fixed)

    return final_hash
}

main :: proc() {
    // Row 1 from Table A
    key1 := Fixed_Key_Part{tenant_id = 42, user_id = 100921}
    dept1 := "engineering"

    // Row 2 from Table B (exact match)
    key2 := Fixed_Key_Part{tenant_id = 42, user_id = 100921}
    dept2 := "engineering"

    // Row 3 from Table C (different department)
    key3 := Fixed_Key_Part{tenant_id = 42, user_id = 100921}
    dept3 := "marketing"

    h1 := hash_composite_join_key(&key1, dept1)
    h2 := hash_composite_join_key(&key2, dept2)
    h3 := hash_composite_join_key(&key3, dept3)

    fmt.printfln("Row 1 Hash: 0x%016x", h1)
    fmt.printfln("Row 2 Hash: 0x%016x (Matches Row 1: %v)", h2, h1 == h2)
    fmt.printfln("Row 3 Hash: 0x%016x (Matches Row 1: %v)", h3, h1 == h3)
}
