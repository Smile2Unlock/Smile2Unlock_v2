//! Mock face-recognition UDP server for VM end-to-end tests.
//!
//! Stands in for su_app.exe (which needs a camera + interactive desktop):
//! binds 127.0.0.1:51236, answers every START_RECOGNITION with a status
//! packet on 127.0.0.1:51234 carrying the SAME session_id (anti-injection
//! check) and a fresh timestamp (freshness check).
//!
//! Usage: mock_recognizer [--fail] [--delay-ms N] [--loop N]
//!   --fail      answer with RS_FAILED instead of RS_SUCCESS
//!   --delay-ms  sleep before answering (simulates recognition time)
//!   --loop      only answer the first N requests, then RS_TIMEOUT behavior
//!               (no reply) -- default: answer forever

use std::net::{Ipv4Addr, UdpSocket};
use std::time::{SystemTime, UNIX_EPOCH};

const AUTH_REQUEST_MAGIC: u32 = 0x4155_5448; // "AUTH"
const AUTH_REQUEST_VERSION: u32 = 1;
const STATUS_MAGIC: u32 = 0x8581_DAF3;
const STATUS_VERSION: u32 = 2;
const REQ_START_RECOGNITION: i32 = 1;
const RS_SUCCESS: i32 = 2;
const RS_FAILED: i32 = 3;
const CP_STATUS_PORT: u16 = 51234;
const AUTH_REQUEST_PORT: u16 = 51236;

#[repr(C, packed)]
struct UdpAuthRequestPacket {
    magic_number: u32,
    version: u32,
    request_type: i32,
    username_hint: [u8; 64],
    timestamp: u64,
    session_id: u32,
}

#[repr(C, packed)]
struct UdpStatusPacket {
    magic_number: u32,
    version: u32,
    status_code: i32,
    session_id: u32,
    feature_bytes: u32,
    username: [u8; 64],
    feature: [u8; 48 * 1024],
    timestamp: u64,
}

fn now_millis() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut fail = false;
    let mut delay_ms: u64 = 0;
    let mut loop_n: Option<u64> = None;
    let mut i = 1;
    while i < args.len() {
        match args[i].as_str() {
            "--fail" => fail = true,
            "--delay-ms" => {
                i += 1;
                delay_ms = args.get(i).and_then(|v| v.parse().ok()).unwrap_or(0);
            }
            "--loop" => {
                i += 1;
                loop_n = args.get(i).and_then(|v| v.parse().ok());
            }
            other => {
                eprintln!("unknown arg: {}", other);
                std::process::exit(2);
            }
        }
        i += 1;
    }

    let sock = match UdpSocket::bind((Ipv4Addr::LOCALHOST, AUTH_REQUEST_PORT)) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("mock_recognizer: bind :{} failed: {}", AUTH_REQUEST_PORT, e);
            std::process::exit(1);
        }
    };
    eprintln!(
        "mock_recognizer: listening on 127.0.0.1:{} fail={} delay_ms={} loop={:?}",
        AUTH_REQUEST_PORT, fail, delay_ms, loop_n
    );

    let mut served: u64 = 0;
    let mut buf = vec![0u8; 256];
    loop {
        match sock.recv_from(&mut buf) {
            Ok((n, src)) => {
                if n < 20 {
                    eprintln!("mock: short packet {} bytes", n);
                    continue;
                }
                let magic = u32::from_le_bytes(buf[0..4].try_into().unwrap());
                let version = u32::from_le_bytes(buf[4..8].try_into().unwrap());
                let req_type = i32::from_le_bytes(buf[8..12].try_into().unwrap());
                // C++ layout: magic(4) ver(4) type(4) username[64] ts(8) session(4)
                // -> session_id lives at byte 84.
                let session_id = u32::from_le_bytes(buf[84..88].try_into().unwrap());
                eprintln!(
                    "mock: req magic={:08x} ver={} type={} session={} from={}",
                    magic, version, req_type, session_id, src
                );
                if magic != AUTH_REQUEST_MAGIC || version != AUTH_REQUEST_VERSION {
                    eprintln!("mock: bad magic/version, ignoring");
                    continue;
                }
                if req_type != REQ_START_RECOGNITION {
                    eprintln!("mock: non-START request ({}), ignoring", req_type);
                    continue;
                }
                if let Some(n) = loop_n {
                    if served >= n {
                        eprintln!("mock: loop limit reached, staying silent");
                        continue;
                    }
                }
                served += 1;
                if delay_ms > 0 {
                    std::thread::sleep(std::time::Duration::from_millis(delay_ms));
                }
                let status = if fail { RS_FAILED } else { RS_SUCCESS };
                let mut pkt = UdpStatusPacket {
                    magic_number: STATUS_MAGIC,
                    version: STATUS_VERSION,
                    status_code: status,
                    session_id,
                    feature_bytes: 0,
                    username: [0u8; 64],
                    feature: [0u8; 48 * 1024],
                    timestamp: now_millis(),
                };
                let bytes = unsafe {
                    core::slice::from_raw_parts(
                        (&pkt as *const UdpStatusPacket).cast::<u8>(),
                        core::mem::size_of::<UdpStatusPacket>(),
                    )
                };
                match sock.send_to(bytes, (Ipv4Addr::LOCALHOST, CP_STATUS_PORT)) {
                    Ok(_) => eprintln!(
                        "mock: sent status={} session={} to :{}",
                        status, session_id, CP_STATUS_PORT
                    ),
                    Err(e) => eprintln!("mock: send_to failed: {}", e),
                }
            }
            Err(e) => {
                eprintln!("mock: recv error: {}", e);
                std::thread::sleep(std::time::Duration::from_millis(200));
            }
        }
    }
}
