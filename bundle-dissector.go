package main

import "fmt"
import "encoding/json"
import "os"
// import "io/ioutil"
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


func GetTxHeader(txId string) (TxHeader) {
	node := "http://gateway-2-temp.arweave.net:1984"

	resp, err := http.Get(node + "/tx/" + txId);
	if err != nil {
		panic("not found");
	}
	defer resp.Body.Close();

	dec := json.NewDecoder(resp.Body)
	var txHeader TxHeader;
	for dec.More() {
		err := dec.Decode(&txHeader)
		if err != nil {
			panic("error during parsing")
		}
	}
	fmt.Printf("%+v\n", txHeader)
	return txHeader;
}
