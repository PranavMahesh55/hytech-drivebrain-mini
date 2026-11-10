#!/usr/bin/env python3
"""Run a real process, subscribe via Foxglove WebSocket, decode its MCAP.
Requires: pip install mcap mcap-protobuf-support websockets
Usage: python tests/integration.py build-native/mini-drivebrain
"""
import asyncio
import json
from pathlib import Path
import signal
import struct
import subprocess
import sys
import tempfile
from mcap.reader import make_reader
from mcap_protobuf.decoder import DecoderFactory
from websockets.asyncio.client import connect

async def receive_live(process):
    for _ in range(100):
        if process.poll() is not None:
            raise AssertionError("Process exited before WebSocket became ready")
        try:
            ws = await connect('ws://127.0.0.1:5555', subprotocols=['foxglove.websocket.v1'])
            break
        except OSError:
            await asyncio.sleep(.05)
    else:
        raise AssertionError("WebSocket never started")
    async with ws:
        while True:
            payload = await asyncio.wait_for(ws.recv(), 5)
            if isinstance(payload, str):
                message = json.loads(payload)
                if message.get('op') == 'advertise':
                    channel = next((c for c in message['channels'] if c['topic'] == '/flowmeter'), None)
                    if channel:
                        assert channel['encoding'] == 'protobuf' and channel['schema']
                        await ws.send(json.dumps({'op': 'subscribe', 'subscriptions': [{'id': 42, 'channelId': channel['id']}]}))
            elif payload[0] == 1:
                assert struct.unpack_from('<I', payload, 1)[0] == 42
                assert len(payload) > 13
                return payload[13:]

async def run(binary, sig):
    with tempfile.TemporaryDirectory() as tmp:
        log_path = Path(tmp) / 'process.log'
        with log_path.open('w') as log:
            process = subprocess.Popen([binary, '--simulate', '--hz', '100', '--pulses-per-liter', '100', '--recordings', tmp], stdout=log, stderr=log)
            try:
                live = await receive_live(process)
                await asyncio.sleep(.4)
                process.send_signal(sig)
                code = await asyncio.to_thread(process.wait, 5)
                assert code == 0, log_path.read_text()
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait()
        files = list(Path(tmp).glob('*.mcap'))
        assert len(files) == 1
        with files[0].open('rb') as f:
            reader = make_reader(f, decoder_factories=[DecoderFactory()])
            summary = reader.get_summary()
            assert summary and summary.statistics.message_count > 0, 'MCAP lacks finalized summary'
            records = list(reader.iter_decoded_messages(topics=['/flowmeter']))
            assert len(records) > 5
            counts = [r.decoded_message.pulse_count for r in records]
            assert counts == sorted(counts) and counts[-1] > 10
            for record in records:
                msg = record.decoded_message
                assert msg.simulated and msg.calibrated and msg.missed_pulses == 0
                assert abs(msg.volume_liters - msg.pulse_count / 100) < 1e-9
            # Decode actual WebSocket payload using the same embedded schema.
            decoded_live = records[0].decoded_message.__class__()
            decoded_live.ParseFromString(live)
            assert decoded_live.simulated and decoded_live.calibrated
            assert decoded_live.pulse_count <= counts[-1]
            text = log_path.read_text()
            assert f'Final pulse count: {counts[-1]},' in text
            print(f'PASS {sig.name}: live protobuf, {len(records)} MCAP snapshots, final count {counts[-1]}, finalized replay')

async def main():
    binary = str(Path(sys.argv[1]).resolve())
    for sig in (signal.SIGINT, signal.SIGTERM):
        await run(binary, sig)
    for args in (['--hz', 'nan'], ['--line', '-1'], ['--duration', '0'], ['--unknown', 'x']):
        result = subprocess.run([binary, '--simulate', *args], capture_output=True, timeout=5)
        assert result.returncode != 0, args
    print('PASS invalid command-line arguments')
    with tempfile.TemporaryDirectory() as tmp:
        blocker = Path(tmp) / 'not-a-directory'
        blocker.write_text('block directory creation')
        result = subprocess.run([binary, '--simulate', '--recordings', str(blocker)], capture_output=True, timeout=5)
        assert result.returncode != 0
        result = subprocess.run([binary, '--chip', '/dev/drivebrain-nonexistent', '--recordings', tmp], capture_output=True, timeout=5)
        assert result.returncode != 0
    print('PASS invalid recordings path and unavailable GPIO cleanup')

asyncio.run(main())
