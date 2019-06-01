<?php declare(strict_types=1); ($_SERVER['REQUEST_URI'] == '/favicon.ico') && die;

// :SET
// Size         Offset          Value
// char[3]      0               'CiK' (Sanity)
// char         3               's'   (OP code)
// u8           4               Key length
// u8           5               Tag 0 length
// u8           6               Tag 1 length
// u8           7               Tag 2 length
// u32          8               Value length
// void *       12              (key + tags + value)

function createSetMessage(string $key, string $value, array $tags = []): string
{
    $klen = strlen($key);
    $vlen = strlen($value);
    $tag0 = array_shift($tags);
    $tag1 = array_shift($tags);
    $tag2 = array_shift($tags);
    $head = pack(
        'c3cCC3N',
        0x43, // C
        0x69, // i
        0x4B, // K
        0x73, // s
        $klen,
        ($tag0 !== null) ? strlen($tag0) : 0,
        ($tag1 !== null) ? strlen($tag1) : 0,
        ($tag2 !== null) ? strlen($tag2) : 0,
        $vlen
    );
    return $head . $key . $tag0 . $tag1 . $tag2 . $value;
}

$key = 'min nyckel';
$val = 'mitt värde';
$tags = ['min första tagg', 'min andra tagg', 'etc..'];
$message = createSetMessage($key, $val, $tags);

var_dump($key, $val, $tags);

$sock_addr = 'tcp://127.0.0.1:5555';
$flags = STREAM_CLIENT_CONNECT;
$timeout = 2.5;
$errno = 0;
$errstr = '';
$fd = @stream_socket_client($sock_addr, $errno, $errstr, $timeout, $flags);

var_dump($fd, $errno, $errstr);

$messageSize = strlen($message);

for ($written = 0; $written < $messageSize; $written += $fwrite) {
    $fwrite = fwrite($fd, substr($message, $written));
    if ($fwrite === false || ($fwrite == 0 && $lastFailed)) {
        var_dump('Fail');
    }
    $lastFailed = $fwrite == 0;
}

@fclose($fd);
