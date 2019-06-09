<?php

declare(strict_types=1);

namespace Improove;

ini_set('display_errors', '1');
ini_set('error_reporting', (string) (E_ALL | E_STRICT));

class Zend_Cache
{
    const CLEANING_MODE_ALL              = 'all';
    const CLEANING_MODE_OLD              = 'old';
    const CLEANING_MODE_MATCHING_TAG     = 'matchingTag';
    const CLEANING_MODE_NOT_MATCHING_TAG = 'notMatchingTag';
    const CLEANING_MODE_MATCHING_ANY_TAG = 'matchingAnyTag';
}

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
    const CMD_BYTE_DEL   = 0x64; // 'd'
    const CMD_BYTE_CLR   = 0x63; // 'c'
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
     * Status codes
     */
    const STATUS_OK                     = 0x00;
    const MASK_INTERNAL_ERROR           = 0x10;
    const STATUS_BUG                    = 0x11;
    const STATUS_CONNECTION_CLOSED      = 0x12;
    const STATUS_NETWORK_ERROR          = 0x13;
    const MASK_CLIENT_ERROR             = 0x20;
    const STATUS_PROTOCOL_ERROR         = 0x21;
    const MASK_CLIENT_MESSAGE           = 0x40;
    const STATUS_NOT_FOUND              = 0x41;
    const STATUS_EXPIRED                = 0x42;
    const STATUS_OUT_OF_MEMORY          = 0x43;
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
            if ($e->getCode() & self::MASK_CLIENT_MESSAGE) {
                return false; // Not an actual error
            }
            throw $e;
        }
    }

    public function test(string $id)
    {
        return $this->load($id) !== false;
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
        if (!$this->writeMessage($message)) {
            return false;
        }
        try {
            return ('' === $this->readMessage());
        } catch (\Exception $e) {
            if ($e->getCode() & self::MASK_CLIENT_MESSAGE) {
                return false; // Not an actual error
            }
            throw $e;
        }
    }

    public function remove(string $id): bool
    {
        $message = $this->makeDelMessage($id);
        if (!$this->writeMessage($message)) {
            return false;
        }
        try {
            return ('' === $this->readMessage());
        } catch (\Exception $e) {
            if ($e->getCode() & self::MASK_CLIENT_MESSAGE) {
                return true; // Not an actual error
            }
            throw $e;
        }
    }

    public function clean($mode = Zend_Cache::CLEANING_MODE_ALL, $tags = [])
    {
        $modeMap = [
            Zend_Cache::CLEANING_MODE_ALL              => 0x00,
            Zend_Cache::CLEANING_MODE_OLD              => 0x01,
            Zend_Cache::CLEANING_MODE_MATCHING_TAG     => 0x02,
            Zend_Cache::CLEANING_MODE_NOT_MATCHING_TAG => 0x03,
            Zend_Cache::CLEANING_MODE_MATCHING_ANY_TAG => 0x04
        ];
        if (!isset($modeMap[$mode])) {
            Zend_Cache::throwException('Invalid mode for clean() method');
        }
        $message = $this->makeClrMessage($modeMap[$mode], $tags);
        var_dump($message);
        if (!$this->writeMessage($message)) {
            return false;
        }
        $response = $this->readMessage();
        var_dump($response);
        return true;
    }

    private function formatKey(string $key): string
    {
        if (strlen($key) > 0xFF) {
            return hash('sha512', $key, false);
        }
        return $key;
    }

    private function formatTag(string $tag): string
    {
        if (strlen($tag) > 0xFF) {
            return hash('sha512', $tag, false);
        }
        return $tag;
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
        $key,
        $value,
        $tags = [],
        $specificLifetime = null
    ) {
        // :SET
        // Size         Offset          Value
        // char[3]      0               'CiK' (Sanity)
        // char         3               's'   (OP code)
        // u8           4               Key length
        // u8           5               num tags
        // u8[2]        6               Padding
        // u32          8               Value length
        // u32          12              TTL in seconds
        // void *       16              (key + tags + value)
        $key  = $this->formatKey($key);
        $tags = array_filter($tags);
        $tags = array_unique($tags);
        $tags = array_slice($tags, 0, 0xFF);
        $klen = strlen($key);
        $tlen = count($tags);
        $vlen = strlen($value);
        $head = pack(
            'c3cCC@8NN',
            self::CONTROL_BYTE_1,
            self::CONTROL_BYTE_2,
            self::CONTROL_BYTE_3,
            self::CMD_BYTE_SET,
            $klen,
            $tlen,
            $vlen,
            $specificLifetime === null ? 0xFFFFFFFF : $specificLifetime
        );
        return $head . $key . $this->makeTagMessage($tags) . $value;
    }

    private function makeDelMessage(string $key): string
    {
        // :DEL
        // Size         Offset          Value
        // char[3]      0               'CiK' (Sanity)
        // char         3               'd'   (OP code)
        // u8           4               Key length
        // u8[11]       6               Padding
        // void *       16              (key)
        $key  = $this->formatKey($key);
        $klen = strlen($key);
        $head = pack(
            'c3cC@16',
            self::CONTROL_BYTE_1,
            self::CONTROL_BYTE_2,
            self::CONTROL_BYTE_3,
            self::CMD_BYTE_DEL,
            $klen
        );
        return $head . $key;
    }

    private function makeClrMessage($mode, $tags = [])
    {
        // :CLR
        // Size         Offset          Value
        // char[3]      0               'CiK' (Sanity)
        // char         3               'c'   (OP code)
        // u8           4               mode
        // u8           5               num tags
        // u8[10]       6               Padding
        // void *       10              (tags)
        $tags = array_filter($tags);
        $tags = array_unique($tags);
        $tags = array_slice($tags, 0, 0xFF);
        $head = pack(
            'c3cCC@16',
            self::CONTROL_BYTE_1,
            self::CONTROL_BYTE_2,
            self::CONTROL_BYTE_3,
            self::CMD_BYTE_CLR,
            (int) $mode,
            count($tags)
        );
        return $head . $this->makeTagMessage($tags);
    }

    private function makeTagMessage($tag)
    {
        if (is_array($tag)) {
            return implode('', array_map([$this, 'makeTagMessage'], $tag));
        }
        $tag = $this->formatTag($tag);
        return pack('C', strlen($tag)) . $tag;
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
        $response = @unpack('c3cik/c1status/NsizeOrError', (string) $header);
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
                'CiK returned error code 0x%X',
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

?>
<script>
setTimeout(function () {
    window.location.reload();
}, 1000);
</script>
<?php

$cik = new CiK();

echo 'Set an entry';
$success = $cik->save(
    'Buy our stuff',
    'A message from our sponsors',
    [
        'catalog_product_13',
        'store_1',
        'CONFIG',
        'a', 'b', 'c', 'asdfasdfasdfasdf'
    ],
    10
);

$success = $cik->save(
    'Buy our stuff',
    'Another message',
    [
        'catalog_product_14',
        'store_1',
        'CONFIG'
    ],
    10
);

$success = $cik->save(
    'Buy our stuff',
    'A third message',
    [
        'catalog_product_15',
        'store_2',
        'CONFIG'
    ],
    10
);

var_dump($success);

$cik->clean(Zend_Cache::CLEANING_MODE_MATCHING_ANY_TAG, [
    'CONFIG',
    'a'
]);

die;

echo 'Test the set entry';
$success = $cik->load('A message from our sponsors') == 'Buy our stuff';
var_dump($success);

echo 'Remove the same entry';
$success = $cik->remove('A message from our sponsors');
var_dump($success);

echo 'Test the set entry again';
$success = $cik->load('A message from our sponsors') == 'Buy our stuff';
var_dump($success);

echo 'Remove a non-existent entry';
$success = $cik->remove('This doesn\'t exist');
var_dump($success);

echo 'Remove the already removed entry';
$success = $cik->remove('A message from our sponsors');
var_dump($success);

// $success = $cik->clean('all', [
//     'store_1',
//     'CONFIG'
// ]);

// var_dump($success);

echo 'Set one more entry';
$success = $cik->save(
    'Buy our stuff',
    'A message from our sponsors, take #2',
    [
        'catalog_product_13',
        'store_1',
        ''
    ],
    10
);
var_dump($success);

var_dump(date('Y-m-d H:i:s'));

exit;
$saveTime = 0;
$loadTime = 0;

try {
    $cik = new CiK();

    $totalSize = 0;
    $message = 'It\'s going';
    for ($i = 0; $i < 100; ++$i) {
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
