package video

/*

#cgo pkg-config: libavformat libavcodec libavutil libswscale

# include "decode_video2.h"
*/
import "C"

import (
	"os"
	"os/exec"
	"time"
	"io"
	"fmt"
	"errors"
	"log"
	"encoding/binary"

	"golang.org/x/term"
)

var leaves = map[string]struct{}{

	"ftyp" : {},
	"mvhd" : {},
	"tkhd" : {},
	"tref" : {},
	"pdin" : {},
	"mdhd" : {},
	"hdlr" : {},
	"vmhd" : {},
	"smhd" : {},
	"hmhd" : {},
	"nmhd" : {},
	"dref" : {},
	"mehd" : {},
	"trex" : {},
	"ipmc" : {},
	"tfhd" : {},
	"trun" : {},
	"sdtp" : {},
	"sbgp" : {},
	"subs" : {},
	"elst" : {},
	"tfra" : {},
	"mfro" : {},
	"mdat" : {},
	"free" : {},
	"udta" : {},
	"cprt" : {},
	"iloc" : {},
	"frma" : {},
	"imif" : {},
	"schm" : {},
	"schi" : {},
	"iinf" : {},
	"xml" : {},
	"bxml" : {},
	"pitm" : {},
	"stsd" : {},
	"stts" : {},
	"ctts" : {},
	"stsc" : {},
	"stsz" : {},
	"stz2" : {},
	"stco" : {},
	"sgpd" : {},
	"stdp" : {},
	"padb" : {},
	"stsh" : {},
	"stss" : {},
	"co64" : {},

}

type Box struct {

	BoxType string
	Size int
	Content []byte
	 
}

type DTS struct{
	Count int
	Delta float64
}

type CTS struct{
	Count int
	Offset float64
}

type MetaData struct{

	CodecName string
	TimeBase int
	Height int
	Width int
	Frames int
	DtsEntries []DTS

}
type DecodedImage struct{

	Reader io.Reader
	Height int
	Width int


}

func parseCellSizeSeq(buff []byte, size int) (int, int, error){

	if buff[0] != '\033' ||  buff[1] != '[' || buff[2] != '6' || buff[3] != ';'{
		return 0, 0, errors.New("Invalid sequence")
	}

	height := 0
	width := 0

	i := 0

	for i = 4; buff[i] != ';'; i++{
		
		height = height*10 + int(buff[i]) - int('0')

	}

	for i = i + 1; buff[i] != 't'; i++{

		width = width * 10 + int(buff[i]) - int('0')

	}

	return height, width, nil
}

func getCellSize()(int, int){

	os.Stdout.Write([]byte("\x1b[16t"))

	buf := make([]byte, 64)

	n, err := os.Stdout.Read(buf)

	if err != nil {
		log.Fatal("Unable to read cell size : ", err)
	}

	height, width, err := parseCellSizeSeq(buf, n)

	if err != nil{
		log.Fatal("Unable to parse cell size :", err)
	}

	return height, width
}

func GetTerminalDimensions() (int, int){

	fd := int(os.Stdout.Fd())

	cols, rows, err := term.GetSize(fd)

	if err != nil{
		log.Fatal("Unable to get size of the terminal :", err)
	}

	cellHeight, cellWidth := getCellSize()

	height := rows * cellHeight
	width := cols * cellWidth

	return height, width
}

func SelectStreams(path string) io.Reader{

	cmd := exec.Command("ffprobe", "-show_entries", "stream=codec_name,time_base,height,width" , "-select_streams", "v", "-print_format", "json", path)

	output, err := cmd.StdoutPipe()

	if err != nil{
		fmt.Println("error outputttinggg the pipe : ", err)
		os.Exit(1)
	}

	if err := cmd.Start(); err != nil{
		fmt.Println("error executing the command for timestamps :" ,err)
		os.Exit(1)
	}

	return output
}


func ReadFrames(path string) io.Reader{

	cmd := exec.Command("ffprobe", "-show_frames", "-select_streams", "v", "-of", "json", path)

	reader, err := cmd.StdoutPipe()

	if err != nil{
		fmt.Println("error creating the pipe : ", err)
		os.Exit(1)
	}

	if err := cmd.Start(); err != nil{
		fmt.Println("error executing the command for timestamps :" ,err)
		os.Exit(1)
	}

	return reader
}

func GetMoov(reader io.Reader) Box{

	var box Box 

	buffer := make([]byte, 1024 * 1024 * 12)
	sizeAndType := make([]byte, 0, 8)

	bytesRead := 0

	for {

		n, err := reader.Read(buffer)

		for i := 0; i < n; {

			if len(sizeAndType) < 8{

				sizeAndType = append(sizeAndType, buffer[i])

				if len(sizeAndType) == 8 && string(sizeAndType[4:]) == "moov"{


					box.BoxType = "moov"
					box.Size = int(binary.BigEndian.Uint32(sizeAndType[:4]))
					box.Content = make([]byte, 0, box.Size)
				}

				bytesRead++
				i++
			}

			if len(sizeAndType) == 8{

				readableBytes := min(n - i, int(binary.BigEndian.Uint32(sizeAndType[:4])) - bytesRead) 

				if string(sizeAndType[4:]) == "moov"{

					box.Content = append(box.Content, buffer[i : i + readableBytes]...)
				}

				bytesRead += readableBytes
				i += readableBytes
			}

			if len(sizeAndType) == 8 && bytesRead == int(binary.BigEndian.Uint32(sizeAndType[:4])){

				if string(sizeAndType[4 : ]) == "moov"{
					break
				}

				sizeAndType = sizeAndType[:0]
				bytesRead = 0
			}
		}

		if err != nil{
			break
		}

	}

	return box
}

func Readmdia(boxes []Box) MetaData {

	var metaData MetaData

	fields := map[string][]Box{

		"hdlr" : []Box{},
		"mdhd" : []Box{},
		"stsd" : []Box{},
		"stts" : []Box{},

	}

	for _, box := range boxes{

		fields := ReadBox(box, fields)

		mediaType := Readhdlr(fields["hdlr"][0].Content)

		if mediaType == "vide"{

			timebase := Readmdhd(fields["mdhd"][0].Content)
			codec, height, width := Readstsd(fields["stsd"][0].Content)

			frames, dtsEntries := Readstts(fields["stts"][0].Content)
			
			metaData.CodecName = codec
			metaData.TimeBase = timebase
			metaData.Height = height
			metaData.Width = width
			metaData.DtsEntries = dtsEntries
			metaData.Frames = frames

			break
		}
	}

	return metaData
}

func Readelst(content []byte){

	fmt.Println(content)
}

func Readstts(content []byte) (int, []DTS){

	entryCount := binary.BigEndian.Uint32(content[4 : 8])

	i := 8

	frames := 0
	dtsEntries := make([]DTS, entryCount)

	for j := range entryCount{

		cnt := int(binary.BigEndian.Uint32(content[i : i + 4]))
		delta := float64(binary.BigEndian.Uint32(content[i + 4 : i + 8]))

		dtsEntries[j] = DTS{cnt, delta}
		frames += cnt

		i += 8
	}
	
	return frames, dtsEntries
}

func Readctts(content []byte) []CTS{

	entryCount := binary.BigEndian.Uint32(content[4 : 8])

	i := 8

	ctsEntries := make([]CTS, entryCount)

	for j := range entryCount{

		cnt := int(binary.BigEndian.Uint32(content[i : i + 4]))
		offset := float64(binary.BigEndian.Uint32(content[i + 4 : i + 8]))

		ctsEntries[j] = CTS{cnt, offset}

		i += 8
	}

	return ctsEntries
}

func Readstsd(content []byte) (string, int, int){

	codingName := string(content[12 : 16])
	width := int(binary.BigEndian.Uint16(content[40 : 42]))
	height := int(binary.BigEndian.Uint16(content[42 : 44]))

	return codingName, height, width
}

func Readhdlr(content []byte) string{

	 return string(content[8 : 12])
}

func Readmdhd(content []byte) int{

	version := content[0]

	var timescale int

	if version == 1{

		timescale = int(binary.BigEndian.Uint32(content[20 : 24]))


	}else{

		timescale = int(binary.BigEndian.Uint32(content[12 : 16]))

	}
	
	return timescale
}

func ReadBox(box Box, fields map[string][]Box) map[string][]Box{

	for i := 0; i < len(box.Content); {

		size := int(binary.BigEndian.Uint32(box.Content[i : i + 4]))
		boxType := string(box.Content[i + 4 : i + 8])
		content := box.Content[i + 8 : i + size]

		if _, ok := fields[boxType]; ok{

			fields[boxType] = append(fields[boxType], Box{boxType, size, content})

		}

		if _, exist := leaves[boxType]; !exist {
			
			fields = ReadBox(Box{boxType, size, content}, fields)

		}

		i += size
	}

	return fields
}

func ReadTest(path string){

	start := time.Now()

	file, err := os.Open(path)

	if err != nil{
		os.Exit(122)
	}

	buffer := make([]byte, 20480)

	bytesRead := 0

	for bytesRead < 1024 * 1024 * 100{

		n, err := file.Read(buffer)

		if err != nil{
			break
		}

		bytesRead += n
	}

	fmt.Println(time.Since(start))

	os.Stdin.Read(buffer)
}

func ReadMP4(path string) MetaData{

	file, err := os.Open(path)

	if err != nil{
		os.Exit(122)
	}

	moov := GetMoov(file)

	fields := map[string][]Box{

		"mdia" : []Box{},

	}

	fields = ReadBox(moov, fields)

	metaData := Readmdia(fields["mdia"])

	return metaData
}

func Decode12(mode *C.int) {

	// C.frameLoop(C.CString(""), mode)
}

func Decode(path string) DecodedImage{

	height, width := GetTerminalDimensions()

	scale := fmt.Sprintf("scale=%d:%d", width, height)

	cmd := exec.Command("ffmpeg", "-i", path, "-vf", scale, "-f" , "rawvideo", "-pix_fmt", "rgb24", "-")

	outputReader, err := cmd.StdoutPipe()

	if err != nil{
		fmt.Println("error getting the error : ", err)
		os.Exit(1)
	}

	if err := cmd.Start(); err != nil{
		fmt.Print("Absolute garbage : ", err)
		os.Exit(1)
	}

	return DecodedImage{outputReader, height, width}
}
