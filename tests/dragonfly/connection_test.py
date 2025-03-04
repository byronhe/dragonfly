import random
import pytest
import asyncio
from redis import asyncio as aioredis
from redis.exceptions import ConnectionError as redis_conn_error
import async_timeout

from . import DflyInstance, dfly_args

BASE_PORT = 1111


async def run_monitor_eval(monitor, expected):
    async with monitor as mon:
        count = 0
        max = len(expected)
        while count < max:
            try:
                async with async_timeout.timeout(1):
                    response = await mon.next_command()
                    if "select" not in response["command"].lower():
                        cmd = expected[count]
                        if cmd not in response["command"]:
                            print(f"command {response['command']} != {cmd}")
                            return False
                        else:
                            count = count + 1
            except Exception as e:
                print(f"failed to monitor: {e}")
                return False
    return True


"""
Test issue https://github.com/dragonflydb/dragonfly/issues/756
Monitor command do not return when we have lua script issue
"""


@pytest.mark.asyncio
async def test_monitor_command_lua(async_pool):
    expected = ["EVAL return redis", "EVAL return redis", "SET foo2"]

    conn = aioredis.Redis(connection_pool=async_pool)
    monitor = conn.monitor()

    cmd1 = aioredis.Redis(connection_pool=async_pool)
    future = asyncio.create_task(run_monitor_eval(monitor=monitor, expected=expected))
    await asyncio.sleep(0.1)

    try:
        res = await cmd1.eval(r'return redis.call("GET", "bar")', 0)
        assert False  # this will return an error
    except Exception as e:
        assert "script tried accessing undeclared key" in str(e)

    try:
        res = await cmd1.eval(r'return redis.call("SET", KEYS[1], ARGV[1])', 1, "foo2", "bar2")
    except Exception as e:
        print(f"EVAL error: {e}")
        assert False

    await asyncio.sleep(0.1)
    await future
    status = future.result()
    assert status


"""
Test the monitor command.
Open connection which is used for monitoring
Then send on other connection commands to dragonfly instance
Make sure that we are getting the commands in the monitor context
"""


@pytest.mark.asyncio
async def test_monitor_command(async_pool):
    def generate(max):
        for i in range(max):
            yield f"key{i}", f"value={i}"

    messages = {a: b for a, b in generate(5)}
    assert await run_monitor(messages, async_pool)


def verify_response(monitor_response: dict, key: str, value: str) -> bool:
    if monitor_response is None:
        return False
    if monitor_response["db"] == 1 and monitor_response["client_type"] == "tcp":
        return key in monitor_response["command"] and value in monitor_response["command"]
    else:
        return False


async def process_cmd(monitor, key, value):
    while True:
        try:
            async with async_timeout.timeout(1):
                response = await monitor.next_command()
                if "select" not in response["command"].lower():
                    success = verify_response(response, key, value)
                    if not success:
                        print(f"failed to verify message {response} for {key}/{value}")
                        return (
                            False,
                            f"failed on the verification of the message {response} at {key}: {value}",
                        )
                    else:
                        return True, None
        except asyncio.TimeoutError:
            pass


async def monitor_cmd(mon: aioredis.client.Monitor, messages: dict):
    success = None
    async with mon as monitor:
        try:
            for key, value in messages.items():
                state, msg = await process_cmd(monitor, key, value)
                if not state:
                    return state, msg
            return True, "monitor is successfully done"
        except Exception as e:
            return False, f"stopping monitor on {e}"


async def run_monitor(messages: dict, pool: aioredis.ConnectionPool):
    cmd1 = aioredis.Redis(connection_pool=pool)
    conn = aioredis.Redis(connection_pool=pool)
    monitor = conn.monitor()
    future = asyncio.create_task(monitor_cmd(monitor, messages))
    success = True

    # make sure that the monitor task starts before we're sending anything else!
    await asyncio.sleep(0.01)
    for key, val in messages.items():
        res = await cmd1.set(key, val)
        if not res:
            success = False
            break
    await asyncio.sleep(0.01)
    await future
    status, message = future.result()
    if status and success:
        return True, "successfully completed all"
    else:
        return False, f"monitor result: {status}: {message}, set command success {success}"


"""
Run test in pipeline mode.
This is mostly how this is done with python - its more like a transaction that
the connections is running all commands in its context
"""


@pytest.mark.asyncio
async def test_pipeline_support(async_client):
    def generate(max):
        for i in range(max):
            yield f"key{i}", f"value={i}"

    messages = {a: b for a, b in generate(5)}
    assert await run_pipeline_mode(async_client, messages)


async def reader(channel: aioredis.client.PubSub, messages, max: int):
    message_count = len(messages)
    while message_count > 0:
        try:
            async with async_timeout.timeout(1):
                message = await channel.get_message(ignore_subscribe_messages=True)
                if message is not None:
                    message_count = message_count - 1
                    if message["data"] not in messages:
                        return False, f"got unexpected message from pubsub - {message['data']}"
                await asyncio.sleep(0.01)
        except asyncio.TimeoutError:
            pass
    return True, "success"


async def run_pipeline_mode(async_client: aioredis.Redis, messages):
    pipe = async_client.pipeline(transaction=False)
    for key, val in messages.items():
        pipe.set(key, val)
    result = await pipe.execute()

    print(f"got result from the pipeline of {result} with len = {len(result)}")
    if len(result) != len(messages):
        return False, f"number of results from pipe {len(result)} != expected {len(messages)}"
    elif False in result:
        return False, "expecting to successfully get all result good, but some failed"
    else:
        return True, "all command processed successfully"


"""
Test the pipeline command
Open connection to the subscriber and publish on the other end messages
Make sure that we are able to send all of them and that we are getting the
expected results on the subscriber side
"""


@pytest.mark.asyncio
async def test_pubsub_command(async_client):
    def generate(max):
        for i in range(max):
            yield f"message number {i}"

    messages = [a for a in generate(5)]
    assert await run_pubsub(async_client, messages, "channel-1")


async def run_pubsub(async_client, messages, channel_name):
    pubsub = async_client.pubsub()
    await pubsub.subscribe(channel_name)

    future = asyncio.create_task(reader(pubsub, messages, len(messages)))
    success = True

    for message in messages:
        res = await async_client.publish(channel_name, message)
        if not res:
            success = False
            break

    await future
    status, message = future.result()

    await pubsub.close()
    if status and success:
        return True, "successfully completed all"
    else:
        return (
            False,
            f"subscriber result: {status}: {message},  publisher publish: success {success}",
        )


async def run_multi_pubsub(async_client, messages, channel_name):
    subs = [async_client.pubsub() for i in range(5)]
    for s in subs:
        await s.subscribe(channel_name)

    tasks = [
        asyncio.create_task(reader(s, messages, random.randint(0, len(messages)))) for s in subs
    ]

    success = True

    for message in messages:
        res = await async_client.publish(channel_name, message)
        if not res:
            success = False
            break

    for f in tasks:
        await f
    results = [f.result() for f in tasks]

    for s in subs:
        await s.close()
    if success:
        for status, message in results:
            if not status:
                return False, f"failed to process {message}"
        return True, "success"
    else:
        return False, "failed to publish"


"""
Test with multiple subscribers for a channel
We want to stress this to see if we have any issue
with the pub sub code since we are "sharing" the message
across multiple connections internally
"""


@pytest.mark.asyncio
async def test_multi_pubsub(async_client):
    def generate(max):
        for i in range(max):
            yield f"this is message number {i} from the publisher on the channel"

    messages = [a for a in generate(500)]
    state, message = await run_multi_pubsub(async_client, messages, "my-channel")

    assert state, message


@pytest.mark.asyncio
async def test_subscribers_with_active_publisher(df_server: DflyInstance, max_connections=100):
    # TODO: I am not how to customize the max connections for the pool.
    async_pool = aioredis.ConnectionPool(
        host="localhost",
        port=df_server.port,
        db=0,
        decode_responses=True,
        max_connections=max_connections,
    )

    async def publish_worker():
        client = aioredis.Redis(connection_pool=async_pool)
        for i in range(0, 2000):
            await client.publish("channel", f"message-{i}")
        await client.close()

    async def channel_reader(channel: aioredis.client.PubSub):
        for i in range(0, 150):
            try:
                async with async_timeout.timeout(1):
                    message = await channel.get_message(ignore_subscribe_messages=True)
            except asyncio.TimeoutError:
                break

    async def subscribe_worker():
        client = aioredis.Redis(connection_pool=async_pool)
        pubsub = client.pubsub()
        async with pubsub as p:
            await pubsub.subscribe("channel")
            await channel_reader(pubsub)
            await pubsub.unsubscribe("channel")

    # Create a publisher that sends constantly messages to the channel
    # Then create subscribers that will subscribe to already active channel
    pub_task = asyncio.create_task(publish_worker())
    await asyncio.gather(*(subscribe_worker() for _ in range(max_connections - 10)))
    await pub_task
    await async_pool.disconnect()


async def test_big_command(df_server, size=8 * 1024):
    reader, writer = await asyncio.open_connection("127.0.0.1", df_server.port)

    writer.write(f"SET a {'v'*size}\n".encode())
    await writer.drain()

    assert "OK" in (await reader.readline()).decode()

    writer.close()
    await writer.wait_closed()


async def test_subscribe_pipelined(async_client: aioredis.Redis):
    pipe = async_client.pipeline(transaction=False)
    pipe.execute_command("subscribe channel").execute_command("subscribe channel")
    await pipe.echo("bye bye").execute()


async def test_subscribe_in_pipeline(async_client: aioredis.Redis):
    pipe = async_client.pipeline(transaction=False)
    pipe.echo("one")
    pipe.execute_command("SUBSCRIBE ch1")
    pipe.echo("two")
    pipe.execute_command("SUBSCRIBE ch2")
    pipe.echo("three")
    res = await pipe.execute()

    assert res == ["one", ["subscribe", "ch1", 1], "two", ["subscribe", "ch2", 2], "three"]


"""
This test makes sure that Dragonfly can receive blocks of pipelined commands even
while a script is still executing. This is a dangerous scenario because both the dispatch fiber
and the connection fiber are actively using the context. What is more, the script execution injects
its own custom reply builder, which can't be used anywhere else, besides the lua script itself.
"""

BUSY_SCRIPT = """
for i=1,300 do
    redis.call('MGET', 'k1', 'k2', 'k3')
end
"""

PACKET1 = """
MGET s1 s2 s3
EVALSHA {sha} 3 k1 k2 k3
"""

PACKET2 = """
MGET m1 m2 m3
MGET m4 m5 m6
MGET m7 m8 m9\n
"""

PACKET3 = (
    """
PING
"""
    * 500
    + "ECHO DONE\n"
)


async def test_parser_while_script_running(async_client: aioredis.Redis, df_server: DflyInstance):
    sha = await async_client.script_load(BUSY_SCRIPT)

    # Use a raw tcp connection for strict control of sent commands
    # Below we send commands while the previous ones didn't finish
    reader, writer = await asyncio.open_connection("localhost", df_server.port)

    # Send first pipeline packet, last commands is a long executing script
    writer.write(PACKET1.format(sha=sha).encode())
    await writer.drain()

    # Give the script some time to start running
    await asyncio.sleep(0.01)

    # Send another packet that will be received while the script is running
    writer.write(PACKET2.encode())
    # The last batch has to be big enough, so the script will finish before it is fully consumed
    writer.write(PACKET3.encode())
    await writer.drain()

    await reader.readuntil(b"DONE")
    writer.close()
    await writer.wait_closed()


@dfly_args({"proactor_threads": 1})
async def test_large_cmd(async_client: aioredis.Redis):
    MAX_ARR_SIZE = 65535
    res = await async_client.hset(
        "foo", mapping={f"key{i}": f"val{i}" for i in range(MAX_ARR_SIZE // 2)}
    )
    assert res == MAX_ARR_SIZE // 2

    res = await async_client.mset({f"key{i}": f"val{i}" for i in range(MAX_ARR_SIZE // 2)})
    assert res

    res = await async_client.mget([f"key{i}" for i in range(MAX_ARR_SIZE)])
    assert len(res) == MAX_ARR_SIZE


@pytest.mark.asyncio
async def test_reject_non_tls_connections_on_tls(with_tls_server_args, df_local_factory):
    server = df_local_factory.create(
        no_tls_on_admin_port="true", admin_port=1111, port=1211, **with_tls_server_args
    )
    server.start()

    client = aioredis.Redis(port=server.port)
    try:
        await client.execute_command("DBSIZE")
    except redis_conn_error:
        pass

    client = aioredis.Redis(port=server.admin_port)
    assert await client.dbsize() == 0
    await client.close()


@pytest.mark.asyncio
async def test_tls_insecure(with_ca_tls_server_args, with_tls_client_args, df_local_factory):
    server = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = aioredis.Redis(port=server.port, **with_tls_client_args, ssl_cert_reqs=None)
    assert await client.dbsize() == 0
    await client.close()


@pytest.mark.asyncio
async def test_tls_full_auth(with_ca_tls_server_args, with_ca_tls_client_args, df_local_factory):
    server = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = aioredis.Redis(port=server.port, **with_ca_tls_client_args)
    assert await client.dbsize() == 0
    await client.close()


@pytest.mark.asyncio
async def test_tls_reject(with_ca_tls_server_args, with_tls_client_args, df_local_factory):
    server = df_local_factory.create(port=BASE_PORT, **with_ca_tls_server_args)
    server.start()

    client = aioredis.Redis(port=server.port, **with_tls_client_args, ssl_cert_reqs=None)
    try:
        await client.ping()
    except redis_conn_error:
        pass

    client = aioredis.Redis(port=server.port, **with_tls_client_args)
    try:
        assert await client.dbsize() != 0
    except redis_conn_error:
        pass

    client = aioredis.Redis(port=server.port, ssl_cert_reqs=None)
    try:
        assert await client.dbsize() != 0
    except redis_conn_error:
        pass
    await client.close()
