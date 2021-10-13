package main

import "fmt"
import "github.com/valyala/fastjson"
import "log"
import "os"
import "io/ioutil"
// import "bytes"
import "net/http"
// import "github.com/vladimirvivien/automi/stream"

type TxHeaderTag struct {
	name string
	value string
}

type TxHeader struct {
	format int
	data_root string
	tags []TxHeaderTag
}



func GetTxHeader(txId string) string {
	node := "http://gateway-2-temp.arweave.net:1984"

	resp, err := http.Get(node + "/tx/" + txId);
	if err != nil {
		panic("not found");
	}
	defer resp.Body.Close();

	body, err := ioutil.ReadAll(resp.Body);

	if err != nil {
		panic("body unreadable");
	}

	var parser fastjson.Parser
	parsedJson, parseErr := parser.Parse(string(body))

	if parseErr != nil {
		log.Fatal(parseErr)
	}

	dataRoot := string(parsedJson.GetStringBytes("data_root"))

	tags := parsedJson.GetArray("tags")

	var validAns104Format = false
	var validAns104Version = false

	for _, tag := range tags {
		tagName := string(tag.GetStringBytes("name"))
		tagValue := string(tag.GetStringBytes("value"))

		// tagPair :: Bundle-Format: binary
		if (tagName == "QnVuZGxlLUZvcm1hdA" && tagValue == "YmluYXJ5") {
			validAns104Format = true
		}

		// tagPair :: Bundle-Version: 2.0.0
		if (tagName == "QnVuZGxlLVZlcnNpb24" && tagValue == "Mi4wLjA") {
			 validAns104Version = true
		}
	}

	if (!validAns104Format || !validAns104Version) {
		log.Fatal("The provided tx is not a valid ans104 bundle format")
	}

	return dataRoot
}


func main() {
	fmt.Println("hello world!")
	txIds := os.Args[1:]

	txHeader := GetTxHeader(txIds[0])
	fmt.Printf("%+v\n", txHeader)

	//"GET",
	// node + "/tx/" + "VSkuqLKJiA9vIHA_38cg3zMdFVIe7aaAUCJXLKuPFFU",
	// func(resp http.ResponseWriter, req *http.Request) {
	// 	fmt.Println("hello http")
	// 	strm := stream.New(req.Body)

	// 	sink := new(bytes.Buffer)

	// 	if err := <-strm.Open(); err != nil {
	// 		fmt.Println(err)
	// 		return
	// 	}

	// 	fmt.Println(sink.String())
	// },
	//)

}
