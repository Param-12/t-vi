package main

// # include "video/decode_video3.h"
import "C"

import (
	"encoding/base64"
	"fmt"
	"os"
	"log"
	"io"
	"sync"
	"time"
	"image"
	_ "image/jpeg"
	_ "image/png"
	"path/filepath"

	"video-renderer-v2/video"
	

	"golang.org/x/term"

	"github.com/tmthrgd/go-shm"
)

const enterAltMode = "\033[?1049h"
const exitAltMode = "\033[?1049l"

type ImageMetaData struct{

	Name string
	Path string
	Width int
	Height int
	format string

}

func ActivateRawMode() (int, *term.State){

	fd := int(os.Stdout.Fd())

	oldState, err := term.MakeRaw(fd)

	if err != nil{
		log.Fatal("Can't make it raw :", err)
	}

	return fd, oldState
}

func DeactivateRawMode(fd int, oldState *term.State){
	term.Restore(fd, oldState)
}

func RenderRGBFile(fileName string, height, width int){

	wd, err := os.Getwd()

	if err != nil{
		log.Fatal("Unable to get the path of present working directory :", err)
	}

	absPath := filepath.Join(wd, fileName)

	payload := base64.StdEncoding.EncodeToString([]byte(absPath))

	fmt.Printf("\033_Gf=24,a=T,t=f,s=%d,v=%d;%s\033\\", width, height, payload)
}

func DeleteAllImagesFromScreen(){

	fmt.Print("\033_Ga=d\033\\")
}

func DeleteFrameWithId(frameId int){

	fmt.Printf("\033_Ga=d,d=I,i=%d,q=1\033\\", frameId)
}

func CreateSharedImage(name string, rgb []byte){

	file, err := os.Create(name)

	if err != nil{
		log.Fatal("unable to crearte file :", err)
	}

	defer file.Close()

	file.Write(rgb)

}

func ImageInfoFromDirectory(path string) []ImageMetaData{

	entries, err := os.ReadDir(path)

	if err != nil{
		log.Fatal("Unable to read the current directory")
	}

	images := make([]ImageMetaData, 0)

	for i, entry := range entries{

		if entry.IsDir(){

			metaData := ImageMetaData{entry.Name(), filepath.Join(path, entry.Name()), 0, 0, "dir"}
			images = append(images, metaData)
			continue
		}
		
		file, err := os.Open(filepath.Join(path, entry.Name()))

		if err != nil{
			log.Fatal("Error reading the", i + 1, "th file in the given directory : ", err)
		}

		config, format, err := image.DecodeConfig(file)

		if err == nil{
			metaData := ImageMetaData{entry.Name(), filepath.Join(path, entry.Name()), config.Width, config.Height, format}

			images =  append(images, metaData)
		}

	}

	return images
}

func WaitForAction() int{

	buffer := make([]byte, 8)

	n, err := os.Stdin.Read(buffer)

	if err != nil{
		log.Fatal("unable to read input :", err)
	}

	if buffer[0] == 3{
		return 0
	}

	if n == 3 && buffer[0] == 27 && buffer [1] == 91 && buffer[2] == 67 {
		return 1
	}

	if n == 3 && buffer[0] == 27 && buffer [1] == 91 && buffer[2] == 68 {
		return 2
	}

	if n == 3 && buffer[0] == 27 && buffer[1] == 91 && buffer[2] == 65{
		return 5
	}

	if n == 3 && buffer[0] == 27 && buffer[1] == 91 && buffer[2] == 66{
		return 6
	}

	if n == 1 && buffer[0] == 13{
		return 3
	}

	if n == 1 && buffer[0] == 27{
		return 4
	}

	if n == 1 && buffer[0] == '-'{
		return 7
	}

	return 12
}

func ParseBaseTime(baseTime string) (float64, float64){

	i := 0

	var numerator float64
	var denominator float64

	numerator = 0
	denominator = 0

	for baseTime[i] != '/'{

		numerator = numerator*10 + float64(int(baseTime[i]) - '0')
		i++
	}

	for i = i + 1; i < len(baseTime); i++{

		denominator = denominator*10 + float64(int(baseTime[i]) - '0')

	}

	return numerator, denominator
}

func GetTimeStamps(metaData video.MetaData) []time.Duration{

	numerator := 1.0
	denominator := float64(metaData.TimeBase)

	timeStamps := make([]time.Duration, metaData.Frames + 1)

	frame := 1

	for _, dts := range metaData.DtsEntries{

		for range dts.Count{

			timeStamps[frame] = timeStamps[frame - 1] + time.Duration(float64(time.Second) * ((dts.Delta * numerator) / denominator))
			frame++
		}
	}

	return timeStamps
}

func CreateSharedImage2(name string, framePixels []byte) {

	file, err := shm.Open(name, os.O_CREATE|os.O_RDWR, 0666)

	if err != nil{
		log.Fatal("unable to initialise shared memory file : ", err)
	}

	file.Truncate(int64(len(framePixels)))

	defer file.Close()

	file.Write(framePixels)
}

func RenderRGBFile2(name string, height, width int){

	payload := base64.StdEncoding.EncodeToString([]byte(name))

	fmt.Printf("\033_Gf=24,a=T,t=s,s=%d,v=%d;%s\033\\", width, height, payload)
}

func RenderRGBFile3(height, width, frame_id int){

	frame_path := fmt.Sprintf("/frame%d", frame_id)

	payload := base64.StdEncoding.EncodeToString([]byte(frame_path))

	// fmt.Printf("\033_Gf=24,a=T,t=s,s=%d,v=%d;%s\033\\", 1920, 1080, payload)
	fmt.Print("\033[H")
    fmt.Printf("\033_Gf=24,a=T,t=s,i=%d,q=1,s=%d,v=%d;%s\033\\", frame_id + 1, width, height, payload);
}

func RenderFrame(decodedImage video.DecodedImage){

	fmt.Print("\033[H")

	frameSize := 3 * decodedImage.Height * decodedImage.Width

	buffer := make([]byte, frameSize)

	_, _ = io.ReadFull(decodedImage.Reader, buffer)

	CreateSharedImage2("aa.rgb", buffer)
	RenderRGBFile2("aa.rgb", decodedImage.Height, decodedImage.Width)
}

func RenderVideo(path string,  rendered_cnt, decoded_cnt *C.int, terminalWidth, terminalHeight int){

	pwd, err := os.Getwd()

	if err != nil{
		log.Fatal("Unable to read the directory : ", err)
	}

	absPath := filepath.Join(pwd, path)

	metaData := video.ReadMP4(absPath)

	timeStamps := GetTimeStamps(metaData)

	start := time.Now()

	var elapsedTime time.Duration

	for i, timeStamp := range timeStamps{

		elapsedTime = time.Since(start)

		if elapsedTime < timeStamp{
			time.Sleep(timeStamp - elapsedTime)
		}

		for C.int(i) >= (*decoded_cnt){

			if i == len(timeStamps) - 1{
				break
			}
			time.Sleep(20);
		}

		RenderRGBFile3(terminalHeight, terminalWidth, i % 50)
		go DeleteFrameWithId(((i - 1 + 50 ) % 50) + 1)
		C.increment(rendered_cnt);
	}
}

func main() {

	if len(os.Args) != 2{

		log.Fatal("Wrong number of arguments")
	}

	videoPath := os.Args[1]

	fd, oldState := ActivateRawMode()
	defer DeactivateRawMode(fd, oldState)

	fmt.Print(enterAltMode)
	defer fmt.Print(exitAltMode)

	rendered_cnt := C.createModeVariable()
	decoded_cnt := C.createModeVariable()

	defer C.deleteModeVariable(rendered_cnt);
	defer C.deleteModeVariable(decoded_cnt);

	terminalHeight, terminalWidth := video.GetTerminalDimensions()

	var wg sync.WaitGroup

	wg.Add(1)
	defer wg.Wait()

	go func(){
		C.frameLoop(C.CString(videoPath), rendered_cnt, decoded_cnt, C.int(terminalWidth), C.int(terminalHeight))
		wg.Done()
	}()

	RenderVideo(videoPath, rendered_cnt, decoded_cnt, terminalWidth, terminalHeight)
}
