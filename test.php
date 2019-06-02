<?php

declare(strict_types=1);

namespace Improove;

class CiK //implements Zend_Cache_Backend_Interface
{

    /**#@+
     * Protocol control byte values
     */
    const CONTROL_BYTE_1 = 0x43; // 'C'
    const CONTROL_BYTE_2 = 0x69; // 'i'
    const CONTROL_BYTE_3 = 0x4B; // 'K'
    const CMD_BYTE_SET   = 0x73; // 's'
    const SUCCESS_BYTE   = 0x74; // 't'
    const FAILURE_BYTE   = 0x66; // 'f'
    /**#@-*/

    /** @var resource */
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
        $success = ($success && ('' === $this->readMessage()));
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
            self::CONTROL_BYTE_1,
            self::CONTROL_BYTE_2,
            self::CONTROL_BYTE_3,
            self::CMD_BYTE_SET,
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
            $lastFailed = ($fwrite == 0);
        }
        return true;
    }

    private function readMessage(): string
    {
        $header = fread($this->fd, 8);
        $response = unpack('c3cik/c1status/NsizeOrError', (string) $header);
        if (!is_array($response)) {
            throw new \Exception(sprintf(
                'Failed to parse CiK response header: 0x%s',
                strtoupper(bin2hex($header))
            ));
        }
        if (($response['cik1'] !== self::CONTROL_BYTE_1)
            || ($response['cik2'] !== self::CONTROL_BYTE_2)
            || ($response['cik3'] !== self::CONTROL_BYTE_3)
            || (($response['status'] !== self::SUCCESS_BYTE)
                && ($response['status'] !== self::FAILURE_BYTE))
        ) {
            throw new \Exception(sprintf(
                'Failed to parse CiK response header: 0x%s',
                strtoupper(bin2hex($header))
            ));
        }
        $success = ($response['status'] === self::SUCCESS_BYTE);
        if (!$success) {
            $errorCode = $response['sizeOrError'];
            throw new \Exception(sprintf(
                'CiK returned error code %d',
                $errorCode
            ), $errorCode);
        }
        $payloadSize = $response['sizeOrError'];
        if ($payloadSize === 0) {
            return '';
        }
        $payload = fread($this->fd, $payloadSize);
        if ($payload === false) {
            throw new \Exception(sprintf(
                'Failed to read CiK payload, header was: 0x%s',
                strtoupper(bin2hex($header))
            ));
        }
        return $payload;
    }
}

($_SERVER['REQUEST_URI'] == '/favicon.ico') && die;

header('Content-Type: text/plain');

try {
    $cik = new CiK();
    $key = 'min nyckel';
    $val = sprintf('My PID is %d', getmypid());
    $tags = ['min första tagg', 'min andra tagg', 'etc..'];
    $success = $cik->save($val, $key, $tags, 1337);
    var_dump($success);
    // Store a smaller value. This should reuse the same entry slot internally.
    $success = $cik->save('...', $key, $tags, 10);
    var_dump($success);
    // Store a bigger value. This should release the old entry maybe.
    // At least if it didn't have enough padding to store the bigger value.
    $success = $cik->save($val . str_repeat(' TAKE 2', 100), $key, $tags);
    var_dump($success);
} catch (\Exception $e) {
    echo (string) $e;
}
