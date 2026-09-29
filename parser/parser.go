package parser


import(
	"os"
	"fmt"
	"io"
	"encoding/json"

	// "video-renderer-v2/video"

)

type Frame struct{
	
	MediaType string `json:"media_type"`
	PTS float64 `json:"pts"`
}

type Stream struct{

	CodecName string `json:"codec_name"`
	TimeBase string `json:"time_base"`
	Height int `json:"height"`
	Width int `json:"width"`

}

type TimeStamps struct{
	Frames []Frame `json:"frames"`
}

type MetaData struct{
	CodecName string
	TimeBase string
	Height int
	Width int
}

type AVFrames struct {

	Streams []Stream `json:"streams"`
	Frames []Frame `json:"frames"`

}

func ReadFile(path string) io.Reader{

	reader, err := os.Open(path)

	if err != nil{
		fmt.Println("Unable to read file :", err)
		os.Exit(1)
	}

	return reader 
}

func GetTimeStamps(reader io.Reader) TimeStamps{

	decoder := json.NewDecoder(reader)

	var frames AVFrames

	decoder.Decode(&frames)

	return TimeStamps{frames.Frames}
}

func ParseTimeStamps(reader io.Reader) []Frame{

	decoder := json.NewDecoder(reader)

	var av AVFrames

	decoder.Decode(&av)
	
	return av.Frames
}

func ParseStream(reader io.Reader) MetaData{

	decoder := json.NewDecoder(reader)

	var avFrames AVFrames

	decoder.Decode(&avFrames)

	return MetaData{avFrames.Streams[0].CodecName, avFrames.Streams[0].TimeBase, avFrames.Streams[0].Height, avFrames.Streams[0].Width}
}

// func ParseMoov(moov video.Box) MetaData{
//
//
//
// }

func ParseJson(path string) AVFrames{

	file := ReadFile(path)

	decoder := json.NewDecoder(file)

	var avFrames AVFrames

	decoder.Decode(&avFrames)

	return avFrames
}

