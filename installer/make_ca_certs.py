# Writes ca_certs.h: the certificate authorities the installer trusts when it
# asks GitHub for a new version, so it can check it really is talking to GitHub
# whatever the Vita's own (2017) list of authorities holds.
#   python make_ca_certs.py     (Windows, with openssl on the path, online)
# The roots come from Windows's own list of trusted roots, picked by their
# SHA-256 fingerprints. The one intermediate comes from api.github.com and is
# kept only if it chains to one of those roots.
import hashlib
import os
import ssl
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOTS = {
    'e793c9b02fd8aa13e21c31228accb08119643b749c898964b1746d46c3d4cbd2': 'USERTrust RSA Certification Authority (github.com, api.github.com)',
    '7bb647a62aeeac88bf257aa522d01ffea395e0ab45c73f93f65654ec38f25a06': 'Sectigo Public Server Authentication Root R46 (the same two)',
    '96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6': "ISRG Root X1 (Let's Encrypt: the hosts the release files come from)",
}
INTERMEDIATE = 'Sectigo Public Server Authentication CA DV R36'


def pem_of(der):
    return ssl.DER_cert_to_PEM_cert(der)


def subject(der):
    out = subprocess.run(['openssl', 'x509', '-noout', '-subject', '-enddate'], input=pem_of(der).encode(),
                         capture_output=True, check=True).stdout.decode()
    return ', '.join(line.strip() for line in out.splitlines() if line.strip())


certs = []  # (note, der)
found = {}
for der, encoding, _ in ssl.enum_certificates('ROOT'):
    if encoding == 'x509_asn':
        found[hashlib.sha256(der).hexdigest()] = der
for fingerprint, note in ROOTS.items():
    if fingerprint not in found:
        raise SystemExit('Windows has no root ' + note)
    certs.append((note, found[fingerprint]))

# the intermediate, as api.github.com sends it to a client that only speaks RSA
shown = subprocess.run(['openssl', 's_client', '-connect', 'api.github.com:443', '-servername', 'api.github.com',
                        '-tls1_2', '-cipher', 'ECDHE-RSA-AES128-GCM-SHA256:AES128-GCM-SHA256', '-showcerts'],
                       input=b'', capture_output=True).stdout.decode()
chain = [ssl.PEM_cert_to_DER_cert('-----BEGIN CERTIFICATE-----' + part.split('-----END CERTIFICATE-----')[0] +
                                  '-----END CERTIFICATE-----\n')
         for part in shown.split('-----BEGIN CERTIFICATE-----')[1:]]
middle = [der for der in chain if ('CN=' + INTERMEDIATE) in subject(der).split('notAfter')[0]]
if not middle:
    raise SystemExit("api.github.com didn't send " + INTERMEDIATE)
with tempfile.TemporaryDirectory() as tmp:
    roots_file, middle_file = os.path.join(tmp, 'roots.pem'), os.path.join(tmp, 'middle.pem')
    open(roots_file, 'w').write(''.join(pem_of(der) for _, der in certs))
    open(middle_file, 'w').write(pem_of(middle[0]))
    check = subprocess.run(['openssl', 'verify', '-no-CApath', '-CAfile', roots_file, middle_file], capture_output=True)
    if check.returncode != 0:
        raise SystemExit('the intermediate does not chain to the roots: ' + check.stderr.decode())
certs.append((INTERMEDIATE + ' (so the chain needs no SHA-384 on the Vita)', middle[0]))

lines = ['// Made by make_ca_certs.py: the certificate authorities trusted for GitHub.', '// Public certificates, as PEM text and as DER bytes.',
         '#ifndef CA_CERTS_H', '#define CA_CERTS_H', '', '#define CA_CERT_COUNT %d' % len(certs), '']
for i, (note, der) in enumerate(certs):
    lines.append('// ' + note)
    lines.append('// ' + subject(der))
    lines.append('static const char ca_pem_%d[] =' % i)
    for row in pem_of(der).strip().splitlines():
        lines.append(chr(9) + chr(34) + row + chr(92) + "n" + chr(34))  # a tab and the row as C text
    lines[-1] += ';'
    lines.append('static const unsigned char ca_der_%d[] = {' % i)
    for at in range(0, len(der), 20):
        lines.append('\t' + ''.join('0x%02X,' % b for b in der[at:at + 20]))
    lines.append('};')
    lines.append('')
lines.append('static const char *const ca_pem[CA_CERT_COUNT] = {' + ', '.join('ca_pem_%d' % i for i in range(len(certs))) + '};')
lines.append('static const unsigned char *const ca_der[CA_CERT_COUNT] = {' + ', '.join('ca_der_%d' % i for i in range(len(certs))) + '};')
lines.append('static const unsigned ca_der_size[CA_CERT_COUNT] = {' + ', '.join('sizeof(ca_der_%d)' % i for i in range(len(certs))) + '};')
lines += ['', '#endif', '']
open(os.path.join(HERE, 'ca_certs.h'), 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))
for note, der in certs:
    print(len(der), 'bytes:', note)
