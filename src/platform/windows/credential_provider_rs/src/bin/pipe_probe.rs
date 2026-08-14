//! pipe_probe — connect to the registered LogonSecret pipe and issue a
//! kPrepare so the service's response (or absence) is visible.
//! Runs on the target VM; run as SYSTEM for the full prepare path, or as a
//! regular admin to verify the pipe exists (expect kAccessDenied).

use su_credential_provider::PipeClient;

fn main() {
    let sid: Vec<u16> = "S-1-5-21-2028198983-2841916586-3191050802-500"
        .encode_utf16()
        .collect();
    match PipeClient.prepare(&sid, 2, 0) {
        Ok(pw) => println!("[probe] prepare OK len={}", pw.as_u16_slice().len()),
        Err(e) => println!("[probe] prepare FAILED {:08x}", e.code().0),
    }
}
