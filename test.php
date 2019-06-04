<?php

declare(strict_types=1);

namespace Improove;

ini_set('display_errors', '1');
ini_set('error_reporting', (string) (E_ALL | E_STRICT));

class CiK //implements Zend_Cache_Backend_Interface
{

    /**#@+
     * Protocol control byte values
     */
    const CONTROL_BYTE_1 = 0x43; // 'C'
    const CONTROL_BYTE_2 = 0x69; // 'i'
    const CONTROL_BYTE_3 = 0x4B; // 'K'
    const CMD_BYTE_GET   = 0x67; // 'g'
    const CMD_BYTE_SET   = 0x73; // 's'
    /**#@-*/

    /**#@+
     * Response indicators
     */
    const SUCCESS_BYTE   = 0x74; // 't'
    const FAILURE_BYTE   = 0x66; // 'f'
    /**#@-*/

    /**#@+
     * Flags
     */
    const FLAG_NONE          = 0x00;
    const FLAG_IGNORE_EXPIRY = 0x01;
    /**#@-*/

    /**#@+
     * Error codes
     */
    const ENODATA = 0x3D;
    /**#@-*/

    /** @var resource */
    private $fd;

    public function __construct()
    {
        $addr = 'tcp://127.0.0.1:5555';
        $errno = 0;
        $errstr = '';
        $timeout = 2.5;
        $flags = STREAM_CLIENT_CONNECT;
        $context = stream_context_create(['socket' => ['tcp_nodelay' => true]]);
        $this->fd = @stream_socket_client(
            $addr,
            $errno,
            $errstr,
            $timeout,
            $flags,
            $context
        );
        if (!$this->fd) {
            throw new \Exception($errstr, $errno);
        }
    }

    public function __destruct()
    {
        if ($this->fd) {
            @fclose($this->fd);
        }
    }

    public function setDirectives(array $directives): void
    {
    }

    public function load(string $id, bool $doNotTestCacheValidity = false)
    {
        $message = $this->makeGetMessage($id, $doNotTestCacheValidity);
        if (!$this->writeMessage($message)) {
            return false;
        }
        try {
            return $this->readMessage();
        } catch (\Exception $e) {
            if ($e->getCode() == self::ENODATA) {
                return false;
            }
            throw $e;
        }
    }

    public function test(string $id)
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

    private function formatKey(string $key): string
    {
        if (strlen($key) > 0xFF) {
            return sha1($key);
        }
        return $key;
    }

    private function makeGetMessage(string $key, bool $ignoreExpiry): string
    {
        // :GET
        // Size         Offset          Value
        // char[3]      0               'CiK' (Sanity)
        // char         3               'g'   (OP code)
        // u8           4               Key length
        // u8           5               Flags
        // u8[10]       6               Padding
        // void *       16              (key)
        $key  = $this->formatKey($key);
        $klen = strlen($key);
        $head = pack(
            'c3cCC@16',
            self::CONTROL_BYTE_1,
            self::CONTROL_BYTE_2,
            self::CONTROL_BYTE_3,
            self::CMD_BYTE_GET,
            $klen,
            $ignoreExpiry ? self::FLAG_IGNORE_EXPIRY : self::FLAG_NONE
        );
        return $head . $key;
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
        $key  = $this->formatKey($key);
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
        $remainingSize = $payloadSize;
        $payload = '';

        do {
            $chunk = fread($this->fd, $remainingSize);
            if ($chunk === false) {
                throw new \Exception(sprintf(
                    'Failed to read CiK payload, header was: 0x%s',
                    strtoupper(bin2hex($header))
                ));
            }
            $payload .= $chunk;
            $remainingSize -= strlen($chunk);
        } while ($remainingSize > 0);

        return $payload;
    }
}

($_SERVER['REQUEST_URI'] == '/favicon.ico') && die;

$saveTime = 0;
$loadTime = 0;

try {
    $cik = new CiK();

    $totalSize = 0;
    $message = 'It\'s going';
    for ($i = 0; $i < 1000; ++$i) {
        $key = 'Key is ' . $i;
        $startTime = microtime(true);
        $success = $cik->save($message, $key, [], 10);
        $saveTime += microtime(true) - $startTime;
        if (!$success) {
            echo sprintf(
                '<h1>%s</h1><dl><dt>%s</dt><dd>%s</dd></dl>',
                'Save Failed',
                $key,
                $message
            );
        }
        $startTime = microtime(true);
        $check = $cik->load($key);
        $loadTime += microtime(true) - $startTime;
        if ($check !== $message) {
            echo sprintf(
                '<h1>%s</h1><dl><dt>%s</dt><dd>%s</dd><dt>%s</dt><dd>%s</dd></dl>',
                'Load Failed',
                'GOT',
                $check,
                'EXPECTED',
                $message
            );
        }

        $totalSize += strlen($key . $message);
        $message .= ' and going';
    }

    var_dump('Total size ' . $totalSize);

    $key = 'min nyckel';
    $val = sprintf('My PID is %d', getmypid());
    $tags = ['min första tagg', 'min andra tagg', 'etc..'];
    $success = $cik->save($val, $key, $tags, 10);
    var_dump($success);

    $value = $cik->load($key);
    var_dump($value);
    $value = $cik->load($key, true);
    var_dump($value);
    $value = $cik->load('marklar');
    var_dump($value);

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

echo sprintf('<h1>Save: %f</h1><h1>Load %f</h1>', $saveTime, $loadTime);
