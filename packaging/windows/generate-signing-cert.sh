#!/usr/bin/env bash
# Generate a self-signed code-signing certificate for internal distribution.
#
# The certificate is NOT publicly trusted. A build that embeds its SHA-256
# fingerprint (SMILE2UNLOCK_PINNED_SIGNER_SHA256) accepts exactly this signer
# without a public CA, and packaging/windows/verify-package.sh verifies the
# Authenticode signatures against it via WINDOWS_VERIFY_CA_FILE.
#
# Usage:
#   packaging/windows/generate-signing-cert.sh [--output-dir DIR] [--days N]
#                                              [--subject DN] [--force]
#
# Outputs (under DIR, default the current directory):
#   windows-signing.pem        PEM certificate (public; safe to share)
#   windows-signing.key        PEM private key (secret; never commit)
#   windows-signing-cert.b64   base64 of the certificate for GitHub secrets
#   windows-signing-key.b64    base64 of the private key for GitHub secrets
#
# It also prints the pinned fingerprint. Upload the base64 files with:
#   gh secret set WINDOWS_SIGN_CERTIFICATE_BASE64 --env release-signing \
#       < DIR/windows-signing-cert.b64
#   gh secret set WINDOWS_SIGN_KEY_BASE64 --env release-signing \
#       < DIR/windows-signing-key.b64

set -euo pipefail

output_dir="."
days=3650
subject="/CN=Smile2Unlock Internal Code Signing/"
force=false

usage() {
    cat <<'USAGE'
Usage: packaging/windows/generate-signing-cert.sh [options]

Options:
  --output-dir DIR   Where to write the certificate and key (default: .)
  --days N           Validity in days (default: 3650)
  --subject DN       OpenSSL subject (default: /CN=Smile2Unlock Internal Code Signing/)
  --force            Overwrite existing files
  --help             Show this help
USAGE
}

while (($# > 0)); do
    case "$1" in
        --output-dir) [[ $# -ge 2 ]] || { echo "missing --output-dir value" >&2; exit 2; }; output_dir="$2"; shift 2 ;;
        --days) [[ $# -ge 2 ]] || { echo "missing --days value" >&2; exit 2; }; days="$2"; shift 2 ;;
        --subject) [[ $# -ge 2 ]] || { echo "missing --subject value" >&2; exit 2; }; subject="$2"; shift 2 ;;
        --force) force=true; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

[[ "$days" =~ ^[0-9]+$ && "$days" -gt 0 ]] || { echo "--days must be a positive integer" >&2; exit 2; }
command -v openssl >/dev/null || { echo "openssl is required" >&2; exit 2; }

mkdir -p "$output_dir"
certificate="${output_dir}/windows-signing.pem"
private_key="${output_dir}/windows-signing.key"
certificate_b64="${output_dir}/windows-signing-cert.b64"
private_key_b64="${output_dir}/windows-signing-key.b64"

if [[ "$force" != true ]]; then
    for path in "$certificate" "$private_key" "$certificate_b64" "$private_key_b64"; do
        if [[ -e "$path" ]]; then
            echo "$path already exists; pass --force to overwrite" >&2
            exit 1
        fi
    done
fi

umask 077
openssl req -x509 -newkey rsa:3072 -nodes \
    -keyout "$private_key" -out "$certificate" \
    -days "$days" -subj "$subject" \
    -addext "extendedKeyUsage=codeSigning" \
    -addext "keyUsage=digitalSignature"
chmod 644 "$certificate"
base64 -w0 < "$certificate" > "$certificate_b64"
base64 -w0 < "$private_key" > "$private_key_b64"

pin="$(openssl x509 -in "$certificate" -noout -fingerprint -sha256 \
    | cut -d= -f2 | tr -d ':' | tr 'A-F' 'a-f')"
echo
echo "certificate: ${certificate}"
echo "private key: ${private_key} (secret; never commit)"
echo "SMILE2UNLOCK_PINNED_SIGNER_SHA256=${pin}"
echo
echo "Upload the signing material as release-signing environment secrets:"
echo "  gh secret set WINDOWS_SIGN_CERTIFICATE_BASE64 --env release-signing < ${certificate_b64}"
echo "  gh secret set WINDOWS_SIGN_KEY_BASE64 --env release-signing < ${private_key_b64}"
