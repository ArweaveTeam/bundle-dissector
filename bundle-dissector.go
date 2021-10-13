package main

import "fmt"
import "bytes"
import "net/http"
import "github.com/vladimirvivien/automi/stream"

func main() {
	node := "http://gateway-2-temp.arweave.net:1984"
	http.HandleFunc(
		node + "/tx/" + "VSkuqLKJiA9vIHA_38cg3zMdFVIe7aaAUCJXLKuPFFU",
		func(resp http.ResponseWriter, req *http.Request) {
			strm := stream.New(req.Body)

			sink := new(bytes.Buffer)

			if err := <-strm.Open(); err != nil {
				fmt.Println(err)
				return
			}

			fmt.Println(sink.String())
		},
	)

}
