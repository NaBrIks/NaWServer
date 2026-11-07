package main

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

func cborHead(major byte, value uint64) []byte {
	if value < 24 {
		return []byte{major<<5 | byte(value)}
	}
	if value <= 0xff {
		return []byte{major<<5 | 24, byte(value)}
	}
	if value <= 0xffff {
		result := []byte{major<<5 | 25, 0, 0}
		binary.BigEndian.PutUint16(result[1:], uint16(value))
		return result
	}
	if value <= 0xffffffff {
		result := []byte{major<<5 | 26, 0, 0, 0, 0}
		binary.BigEndian.PutUint32(result[1:], uint32(value))
		return result
	}
	result := make([]byte, 9)
	result[0] = major<<5 | 27
	binary.BigEndian.PutUint64(result[1:], value)
	return result
}

func cborText(value string) []byte {
	result := cborHead(3, uint64(len(value)))
	return append(result, value...)
}

func cborUint(value uint64) []byte { return cborHead(0, value) }

func cborMap(pairs ...[]byte) []byte {
	result := cborHead(5, uint64(len(pairs)/2))
	for _, pair := range pairs {
		result = append(result, pair...)
	}
	return result
}

func echoCBOR(requestID, value string) []byte {
	data := cborMap(cborText("text"), cborText(value))
	return cborMap(
		cborText("v"), cborUint(2),
		cborText("type"), cborText("echo.request"),
		cborText("request_id"), cborText(requestID),
		cborText("data"), data,
	)
}

type cborReader struct {
	data       []byte
	off        int
	additional byte
}

func (reader *cborReader) head() (byte, uint64, error) {
	if reader.off >= len(reader.data) {
		return 0, 0, errors.New("truncated CBOR")
	}
	value := reader.data[reader.off]
	reader.off++
	additional := value & 31
	reader.additional = additional
	var length int
	switch {
	case additional < 24:
		return value >> 5, uint64(additional), nil
	case additional == 24:
		length = 1
	case additional == 25:
		length = 2
	case additional == 26:
		length = 4
	case additional == 27:
		length = 8
	default:
		return 0, 0, fmt.Errorf("unsupported CBOR additional information %d", additional)
	}
	if reader.off+length > len(reader.data) {
		return 0, 0, errors.New("truncated CBOR argument")
	}
	var number uint64
	switch length {
	case 1:
		number = uint64(reader.data[reader.off])
	case 2:
		number = uint64(binary.BigEndian.Uint16(reader.data[reader.off:]))
	case 4:
		number = uint64(binary.BigEndian.Uint32(reader.data[reader.off:]))
	case 8:
		number = binary.BigEndian.Uint64(reader.data[reader.off:])
	}
	reader.off += length
	return value >> 5, number, nil
}

func (reader *cborReader) item(depth int) (any, error) {
	if depth > 24 {
		return nil, errors.New("CBOR nesting limit")
	}
	major, value, err := reader.head()
	if err != nil {
		return nil, err
	}
	switch major {
	case 0:
		return value, nil
	case 1:
		return -1 - int64(value), nil
	case 2, 3:
		if value > uint64(len(reader.data)-reader.off) {
			return nil, errors.New("truncated CBOR string")
		}
		end := reader.off + int(value)
		bytes := reader.data[reader.off:end]
		reader.off = end
		if major == 2 {
			return append([]byte(nil), bytes...), nil
		}
		return string(bytes), nil
	case 4:
		if value > 4096 {
			return nil, errors.New("CBOR array limit")
		}
		array := make([]any, int(value))
		for index := range array {
			array[index], err = reader.item(depth + 1)
			if err != nil {
				return nil, err
			}
		}
		return array, nil
	case 5:
		if value > 4096 {
			return nil, errors.New("CBOR map limit")
		}
		object := make(map[any]any, int(value))
		for range int(value) {
			key, keyErr := reader.item(depth + 1)
			if keyErr != nil {
				return nil, keyErr
			}
			item, itemErr := reader.item(depth + 1)
			if itemErr != nil {
				return nil, itemErr
			}
			object[key] = item
		}
		return object, nil
	case 7:
		switch reader.additional {
		case 20:
			return false, nil
		case 21:
			return true, nil
		case 22:
			return nil, nil
		case 25:
			return decodeHalfFloat(uint16(value)), nil
		case 26:
			return float64(math.Float32frombits(uint32(value))), nil
		case 27:
			return math.Float64frombits(value), nil
		default:
			return nil, fmt.Errorf("unsupported CBOR simple/float additional value %d", reader.additional)
		}
	default:
		return nil, fmt.Errorf("unsupported CBOR major type %d", major)
	}
}

func decodeHalfFloat(bits uint16) float64 {
	sign := 1.0
	if bits&0x8000 != 0 {
		sign = -1
	}
	exponent := (bits >> 10) & 0x1f
	fraction := bits & 0x03ff
	switch exponent {
	case 0:
		return sign * math.Ldexp(float64(fraction), -24)
	case 0x1f:
		if fraction == 0 {
			return sign * math.Inf(1)
		}
		return math.NaN()
	default:
		return sign * math.Ldexp(float64(1024+fraction), int(exponent)-25)
	}
}

func decodeCBOR(data []byte) (any, error) {
	reader := cborReader{data: data}
	value, err := reader.item(0)
	if err == nil && reader.off != len(data) {
		err = errors.New("trailing CBOR bytes")
	}
	return value, err
}

func cborBootID(data []byte) string {
	value, err := decodeCBOR(data)
	if err != nil {
		return ""
	}
	root, ok := value.(map[any]any)
	if !ok {
		return ""
	}
	bootID, _ := root["boot_id"].(string)
	return bootID
}
