//! kgkey: protect an SSH private key with a passphrase, using the age format (scrypt + ChaCha20-Poly1305).
//!
//!   kgkey enc <plain-key-file> <encrypted-file>   prompts twice for a new passphrase (on /dev/tty, no echo)
//!   kgkey dec <encrypted-file>                    prompts once, writes the key to stdout
//!
//! Dropbear's dbclient cannot read passphrase-protected keys, so the Kobo's `ssh` helper keeps the key encrypted at
//! rest and decrypts it into RAM (tmpfs) only while a connection is open. No custom cryptography: all of it is the `age`
//! library. Encrypting on the device calibrates the scrypt work factor to roughly one second on that device.
use age::secrecy::{ExposeSecret, Secret};
use std::io::{Read, Write};
use std::os::unix::fs::OpenOptionsExt;

fn encrypt_bytes(data: &[u8], passphrase: Secret<String>) -> Result<Vec<u8>, String> {
    let encryptor = age::Encryptor::with_user_passphrase(passphrase);
    let mut out = Vec::new();
    let mut w = encryptor.wrap_output(&mut out).map_err(|e| e.to_string())?;
    w.write_all(data).map_err(|e| e.to_string())?;
    w.finish().map_err(|e| e.to_string())?;
    Ok(out)
}

fn decrypt_bytes(data: &[u8], passphrase: &Secret<String>) -> Result<Vec<u8>, String> {
    let decryptor = match age::Decryptor::new(data).map_err(|e| e.to_string())? {
        age::Decryptor::Passphrase(d) => d,
        _ => return Err("not a passphrase-encrypted file".into()),
    };
    let mut reader = decryptor.decrypt(passphrase, None).map_err(|_| "wrong passphrase (or damaged file)".to_string())?;
    let mut out = Vec::new();
    reader.read_to_end(&mut out).map_err(|e| e.to_string())?;
    Ok(out)
}

fn prompt(msg: &str) -> Secret<String> {
    Secret::new(rpassword::prompt_password(msg).unwrap_or_else(|e| { eprintln!("cannot read the passphrase: {e}"); std::process::exit(2) }))
}

fn die(msg: &str) -> ! {
    eprintln!("kgkey: {msg}");
    std::process::exit(1)
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    match a.get(1).map(String::as_str) {
        Some("enc") if a.len() == 4 => {
            let plain = std::fs::read(&a[2]).unwrap_or_else(|e| die(&format!("{}: {e}", a[2])));
            let p1 = prompt("New passphrase: ");
            if p1.expose_secret().is_empty() { die("an empty passphrase would protect nothing; refusing"); }
            let p2 = prompt("Repeat passphrase: ");
            if p1.expose_secret() != p2.expose_secret() { die("the passphrases differ"); }
            eprintln!("encrypting (this takes a few seconds)...");
            let enc = encrypt_bytes(&plain, p1).unwrap_or_else(|e| die(&e));
            let mut f = std::fs::OpenOptions::new().write(true).create(true).truncate(true).mode(0o600).open(&a[3])
                .unwrap_or_else(|e| die(&format!("{}: {e}", a[3])));
            f.write_all(&enc).unwrap_or_else(|e| die(&e.to_string()));
            f.sync_all().ok();
        }
        Some("dec") if a.len() == 3 => {
            let enc = std::fs::read(&a[2]).unwrap_or_else(|e| die(&format!("{}: {e}", a[2])));
            let p = prompt("Key passphrase: ");
            let plain = decrypt_bytes(&enc, &p).unwrap_or_else(|e| die(&e));
            std::io::stdout().write_all(&plain).unwrap_or_else(|e| die(&e.to_string()));
        }
        _ => {
            eprintln!("usage: kgkey enc <plain-key> <encrypted-file> | kgkey dec <encrypted-file>");
            std::process::exit(64);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn roundtrip_and_wrong_passphrase() {
        let key = b"-----BEGIN PRETEND KEY-----\nabc\n";
        let enc = encrypt_bytes(key, Secret::new("correct horse".to_string())).unwrap();
        assert!(!enc.windows(key.len()).any(|w| w == key), "plaintext must not appear in the output");
        assert_eq!(decrypt_bytes(&enc, &Secret::new("correct horse".to_string())).unwrap(), key);
        assert!(decrypt_bytes(&enc, &Secret::new("wrong".to_string())).is_err());
        let mut damaged = enc.clone();
        let n = damaged.len();
        damaged[n - 3] ^= 1;
        assert!(decrypt_bytes(&damaged, &Secret::new("correct horse".to_string())).is_err(), "tampering must be detected");
    }
}
