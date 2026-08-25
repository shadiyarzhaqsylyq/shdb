package main

import "base:runtime"
import "core:c"
import "core:fmt"
import "core:net"
import "core:strings"
//client
//generate certs before compiling it
//openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem -out cert.pem -days 365
foreign import libbearssl "libbearssl.a"


BR_SSL_BUFSIZE_BIDI :: 32538
BR_SSL_CLOSED       :: 0x0001

Read_Proc  :: #type proc "c" (read_context: rawptr, buf: [^]u8, len: c.size_t) -> c.int
Write_Proc :: #type proc "c" (write_context: rawptr, buf: [^]u8, len: c.size_t) -> c.int

SSL_Client_Context :: struct #align (16) {
	_opaque: [4096]u8,
}

X509_Minimal_Context :: struct #align (16) {
	_opaque: [2048]u8,
}

SSLIO_Context :: struct {
	engine:        rawptr,
	read_proc:     Read_Proc,
	read_context:  rawptr,
	write_proc:    Write_Proc,
	write_context: rawptr,
}

@(default_calling_convention = "c")
foreign libbearssl {
	br_ssl_client_init_full :: proc(
		cc: ^SSL_Client_Context,
		xc: ^X509_Minimal_Context,
		trust_anchors: rawptr,
		num_trust_anchors: c.size_t,
	) ---

	br_ssl_engine_set_buffer :: proc(
		engine: rawptr,
		iobuf: rawptr,
		iobuf_len: c.size_t,
		bidi: c.int,
	) ---

	br_ssl_client_reset :: proc(
		cc: ^SSL_Client_Context,
		server_name: cstring,
		resume_session: c.int,
	) -> c.int ---

	br_sslio_init :: proc(
		ctx: ^SSLIO_Context,
		engine: rawptr,
		read_proc: Read_Proc,
		read_ctx: rawptr,
		write_proc: Write_Proc,
		write_ctx: rawptr,
	) ---

	br_sslio_write_all :: proc(ctx: ^SSLIO_Context, src: rawptr, len: c.size_t) -> c.int ---
	br_sslio_flush     :: proc(ctx: ^SSLIO_Context) -> c.int ---
	br_sslio_read      :: proc(ctx: ^SSLIO_Context, dst: rawptr, len: c.size_t) -> c.int ---
	br_sslio_close     :: proc(ctx: ^SSLIO_Context) -> c.int ---

	br_ssl_engine_current_state :: proc(engine: rawptr) -> c.uint ---
}

sock_read :: proc "c" (ctx: rawptr, buf: [^]u8, len: c.size_t) -> c.int {
	context = runtime.default_context()
	sock := (cast(^net.TCP_Socket)ctx)^

	bytes_read, err := net.recv_tcp(sock, buf[:len])
	if err != nil {
		return -1
	}
	return c.int(bytes_read)
}

sock_write :: proc "c" (ctx: rawptr, buf: [^]u8, len: c.size_t) -> c.int {
	context = runtime.default_context()
	sock := (cast(^net.TCP_Socket)ctx)^

	bytes_sent, err := net.send_tcp(sock, buf[:len])
	if err != nil {
		return -1
	}
	return c.int(bytes_sent)
}

main :: proc() {
	host := "example.com"
	address := fmt.tprintf("%s:%d", host, 443)

	fmt.printfln("Connecting to %s...", address)

	sock, dial_err := net.dial_tcp_from_hostname_and_port_string(address)
	if dial_err != nil {
		fmt.printfln("TCP connect failed: %v", dial_err)
		return
	}
	defer net.close(sock)

	sc: SSL_Client_Context
	xc: X509_Minimal_Context
	io_buf: [BR_SSL_BUFSIZE_BIDI]u8
	ioc: SSLIO_Context

	br_ssl_client_init_full(&sc, &xc, nil, 0)
	br_ssl_engine_set_buffer(&sc, &io_buf[0], size_of(io_buf), 1)

	host_c := strings.clone_to_cstring(host)
	defer delete(host_c)
	br_ssl_client_reset(&sc, host_c, 0)

	br_sslio_init(&ioc, &sc, sock_read, &sock, sock_write, &sock)

	http_req := fmt.tprintf("GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", host)
	fmt.println("Sending encrypted HTTP request...")

	if br_sslio_write_all(&ioc, raw_data(http_req), c.size_t(len(http_req))) < 0 {
		state := br_ssl_engine_current_state(&sc)
		fmt.printfln("TLS send failed. Engine state: 0x%04X", state)
		return
	}
	br_sslio_flush(&ioc)

	fmt.println("Receiving TLS response:\n---")
	read_buf: [1024]u8
	for {
		rlen := br_sslio_read(&ioc, &read_buf[0], size_of(read_buf))
		if rlen <= 0 {
			break
		}
		fmt.print(string(read_buf[:rlen]))
	}

	br_sslio_close(&ioc)
	fmt.println("\n--- Connection closed successfully.")
}
