# This file is part of Project SkyFire https://www.projectskyfire.org.
# See LICENSE.md file for Copyright information
"""Real native matchmaking listener with disposable certificates and a mock hub."""
import argparse
import asyncio
import contextlib
import datetime
import ipaddress
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import unittest


def snapshot(sequence=1, realm=1, generation='a'*64):
    payload = struct.pack('!BIH',1,realm,64)+generation.encode()+struct.pack('!QIBIIBBIH',sequence,2,0,2,4,0,0,60000,2)
    for group,team,members in [(10,0,[101,102]),(20,1,[201,202])]:
        payload += struct.pack('!QBBIH',group,team,0,1000,2)+struct.pack('!QQ',*members)
    return payload+struct.pack('!H',0)


class BattlegroundTests(unittest.IsolatedAsyncioTestCase):
    executable = None

    async def asyncSetUp(self):
        if not self.executable: self.skipTest('Pass --battlegroundserver with a compiled daemon')
        from cryptography import x509
        from cryptography.hazmat.primitives import hashes,serialization
        from cryptography.hazmat.primitives.asymmetric import rsa
        from cryptography.x509.oid import NameOID,ExtendedKeyUsageOID
        self.temporary=tempfile.TemporaryDirectory(prefix='skyfire-battleground-test-')
        self.root=Path(self.temporary.name)
        self.process=self.hub=self.log=None
        self.clients=set();self.tasks=set();self.metrics=asyncio.Event()
        self.addAsyncCleanup(self.cleanup)
        now=datetime.datetime.now(datetime.timezone.utc)
        key=rsa.generate_private_key(public_exponent=65537,key_size=2048)
        name=x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,'Battleground fixture CA')])
        ca=(x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now-datetime.timedelta(minutes=1))
            .not_valid_after(now+datetime.timedelta(days=1)).add_extension(x509.BasicConstraints(ca=True,path_length=None),True)
            .sign(key,hashes.SHA256()))
        (self.root/'ca.pem').write_bytes(ca.public_bytes(serialization.Encoding.PEM))
        for identity in ('hub','bg-test','world-a','world-b','outsider'):
            private=rsa.generate_private_key(public_exponent=65537,key_size=2048)
            cert=(x509.CertificateBuilder().subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME,identity)]))
                .issuer_name(name).public_key(private.public_key()).serial_number(x509.random_serial_number())
                .not_valid_before(now-datetime.timedelta(minutes=1)).not_valid_after(now+datetime.timedelta(days=1))
                .add_extension(x509.SubjectAlternativeName([x509.IPAddress(ipaddress.ip_address('127.0.0.1'))]),False)
                .add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.CLIENT_AUTH,ExtendedKeyUsageOID.SERVER_AUTH]),False)
                .sign(key,hashes.SHA256()))
            (self.root/(identity+'.pem')).write_bytes(cert.public_bytes(serialization.Encoding.PEM))
            (self.root/(identity+'.key')).write_bytes(private.private_bytes(serialization.Encoding.PEM,serialization.PrivateFormat.PKCS8,serialization.NoEncryption()))
        tls=ssl.create_default_context(ssl.Purpose.CLIENT_AUTH,cafile=str(self.root/'ca.pem'))
        tls.load_cert_chain(self.root/'hub.pem',self.root/'hub.key');tls.verify_mode=ssl.CERT_REQUIRED
        self.hub=await asyncio.start_server(self.serve_hub,'127.0.0.1',0,ssl=tls)
        hub_port=self.hub.sockets[0].getsockname()[1]
        with socket.socket() as reserve:
            reserve.bind(('127.0.0.1',0));self.port=reserve.getsockname()[1]
        config=f'''[battlegroundserver]
ConfVersion = 2026092900
Battleground.BindIP = "127.0.0.1"
Battleground.Port = {self.port}
Battleground.Realms = "1 2"
Battleground.AllowedWorlds = "world-a world-b"
Battleground.WorldRealms = "world-a=1 world-b=2"
Battleground.RequestTimeout = 2
Cluster.Enable = 1
Cluster.NodeKey = "bg-test"
Cluster.NodeName = "Fixture battleground"
Cluster.HubHost = "127.0.0.1"
Cluster.HubPort = {hub_port}
Cluster.AdvertiseAddress = "127.0.0.1"
Cluster.Certificate = "{(self.root/'bg-test.pem').as_posix()}"
Cluster.PrivateKey = "{(self.root/'bg-test.key').as_posix()}"
Cluster.CA = "{(self.root/'ca.pem').as_posix()}"
Cluster.CRL = ""
Cluster.Handoff.Enable = 0
Cluster.RealmDirectory.Enable = 0
Appender.Console = 1,3,0
Logger.root = 3,Console
'''
        (self.root/'bg.conf').write_text(config)
        self.log=(self.root/'native.log').open('wb')
        self.process=await asyncio.create_subprocess_exec(str(Path(self.executable).resolve()),'-c',str(self.root/'bg.conf'),
            cwd=self.root,stdout=self.log,stderr=self.log,creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
        for _ in range(100):
            if self.process.returncode is not None:
                self.fail('Daemon exited: '+(self.root/'native.log').read_text(errors='replace'))
            try:
                await self.exchange('world-a',1,1)
                break
            except (ConnectionError,asyncio.TimeoutError,asyncio.IncompleteReadError): await asyncio.sleep(.1)
        else: self.fail('Battleground daemon did not register with the hub')

    async def serve_hub(self,reader,writer):
        self.clients.add(writer);task=asyncio.current_task();self.tasks.add(task)
        try:
            while True:
                magic,version,kind,size=struct.unpack('!4sHHI',await reader.readexactly(12))
                if magic!=b'SFHC' or version!=1 or size>4096: return
                payload=await reader.readexactly(size)
                if kind==15:
                    self.assertEqual(payload[0],1);self.metrics.set()
                reply=struct.pack('!HI',kind,5)
                writer.write(b'SFHC'+struct.pack('!HHI',1,0x8000,len(reply))+reply);await writer.drain()
        except (ConnectionError,asyncio.IncompleteReadError): pass
        finally:
            writer.close();self.clients.discard(writer);self.tasks.discard(task)

    async def exchange(self,identity,realm,operation,body=b'',accepted=True):
        tls=ssl.create_default_context(cafile=str(self.root/'ca.pem'));tls.load_cert_chain(self.root/(identity+'.pem'),self.root/(identity+'.key'))
        reader,writer=await asyncio.wait_for(asyncio.open_connection('127.0.0.1',self.port,ssl=tls),4)
        header=b'SFBG'+struct.pack('!HHII',1,operation,1,realm)
        try:
            writer.write(header+(struct.pack('!I',len(body))+body if operation==2 else b''));await asyncio.wait_for(writer.drain(),4)
            if not accepted:
                self.assertEqual(await asyncio.wait_for(reader.read(1),4),b'');return
            self.assertEqual(await asyncio.wait_for(reader.readexactly(16),4),header[:6]+struct.pack('!H',0x8000|operation)+header[8:])
            if operation==2:
                size=struct.unpack('!I',await asyncio.wait_for(reader.readexactly(4),4))[0]
                self.assertLessEqual(size,128*1024)
                return await asyncio.wait_for(reader.readexactly(size),4)
        except (ConnectionResetError,ssl.SSLError):
            if accepted: raise
        finally:
            writer.close()
            with contextlib.suppress(OSError,asyncio.TimeoutError): await asyncio.wait_for(writer.wait_closed(),2)

    async def test_matching_and_fencing(self):
        await self.exchange('outsider',1,1,accepted=False)
        await self.exchange('world-b',1,1,accepted=False)
        expected=struct.pack('!BQHIHQHQ',1,1,1,0,1,10,1,20)
        self.assertEqual(await self.exchange('world-a',1,2,snapshot()),expected)
        await self.exchange('world-a',1,2,snapshot(),accepted=False)
        await self.exchange('world-a',1,2,snapshot(2,realm=2),accepted=False)
        await self.exchange('world-a',1,2,snapshot(2,generation='c'*64),accepted=False)
        await self.exchange('world-a',1,2,snapshot(2)+b'\0',accepted=False)
        self.assertEqual(await self.exchange('world-b',2,2,snapshot(realm=2,generation='b'*64)),expected)
        await asyncio.wait_for(self.metrics.wait(),10)
        self.hub.close();await self.hub.wait_closed()
        for writer in list(self.clients): writer.close()
        await asyncio.sleep(2)
        await self.exchange('world-a',1,1,accepted=False)

    async def cleanup(self):
        if self.process and self.process.returncode is None:
            self.process.kill();await self.process.wait()
        if self.hub: self.hub.close();await self.hub.wait_closed()
        for writer in list(self.clients): writer.close()
        if self.tasks: await asyncio.gather(*list(self.tasks),return_exceptions=True)
        if self.log: self.log.close()
        self.temporary.cleanup()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--battlegroundserver');args,remaining=parser.parse_known_args()
    BattlegroundTests.executable=args.battlegroundserver
    unittest.main(argv=[sys.argv[0]]+remaining)
