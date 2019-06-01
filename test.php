<?php

declare(strict_types=1);

namespace Improove;

class CiK //implements Zend_Cache_Backend_Interface
{

    private $fd;

    public function __construct()
    {
        $addr = 'tcp://127.0.0.1:5555';
        $flags = STREAM_CLIENT_CONNECT;
        $timeout = 2.5;
        $errno = 0;
        $errstr = '';
        $this->fd = @stream_socket_client($addr, $errno, $errstr, $timeout, $flags);
        if (!$this->fd) {
            throw new \Exception($errstr, $errno);
        }
    }

    public function __destruct()
    {
        @fclose($this->fd);
    }

    public function setDirectives(array $directives): void
    {
    }

    public function load(string $id, bool $doNotTestCacheValidity = false): ?string
    {
        return false;
    }

    public function test(string $id): ?int
    {
        return false;
    }

    public function save(
        string $data,
        string $id,
        array $tags = [],
        $specificLifetime = false
    ): bool {

        $message = $this->makeSetMessage(
            $id,
            $data,
            $tags,
            $specificLifetime === false ? null : (int) $specificLifetime
        );
        $success = $this->writeMessage($message);
        return $success;
    }

    public function remove(string $id): bool
    {
        return false;
    }

    public function clean(string $mode = 'all', array $tags = []): bool
    {
        return false;
    }

    private function makeSetMessage(
        string $key,
        string $value,
        array $tags = [],
        int $specificLifetime = null
    ): string {

        // :SET
        // Size         Offset          Value
        // char[3]      0               'CiK' (Sanity)
        // char         3               's'   (OP code)
        // u8           4               Key length
        // u8           5               Tag 0 length
        // u8           6               Tag 1 length
        // u8           7               Tag 2 length
        // u32          8               Value length
        // u32          12              TTL in seconds
        // void *       16              (key + tags + value)
        $klen = strlen($key);
        $vlen = strlen($value);
        $tag0 = array_shift($tags);
        $tag1 = array_shift($tags);
        $tag2 = array_shift($tags); // We support at most 3 tags
        $head = pack(
            'c3cCC3NN',
            0x43, // C
            0x69, // i
            0x4B, // K
            0x73, // s
            $klen,
            ($tag0 !== null) ? strlen($tag0) : 0,
            ($tag1 !== null) ? strlen($tag1) : 0,
            ($tag2 !== null) ? strlen($tag2) : 0,
            $vlen,
            $specificLifetime === null ? 0xFFFFFFFF : $specificLifetime
        );
        return $head . $key . $tag0 . $tag1 . $tag2 . $value;
    }

    private function writeMessage(string $message): bool
    {
        $messageSize = strlen($message);
        for ($written = 0; $written < $messageSize; $written += $fwrite) {
            $fwrite = fwrite($this->fd, substr($message, $written));
            if ($fwrite === false || ($fwrite == 0 && $lastFailed)) {
                return false;
            }
            $lastFailed = $fwrite == 0;
        }
        return true;
    }
}

($_SERVER['REQUEST_URI'] == '/favicon.ico') && die;

try {
    $cik = new CiK();
    $key = 'min nyckel';
    $val = sprintf('My PID is %d', getmypid());
    $tags = ['min första tagg', 'min andra tagg', 'etc..'];
    $cik->save($val, $key, $tags, 1337);
    $cik->save($val . ' TAKE 2', $key, $tags);
} catch (\Exception $e) {
    echo (string) $e;
}
