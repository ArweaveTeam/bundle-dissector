#[macro_use]
extern crate lazy_static;

use anyhow::{anyhow, Result};
use regex::Regex;
use reqwest::get;
use serde::{Deserialize, Serialize};
use std::{convert::TryInto, fs};
use tokio::io::{self, AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;
use futures::future::poll_fn;

const NODE_URL: &str = "http://gateway-2-temp.arweave.net:1984";

#[derive(Deserialize, Debug)]
struct TxHeader {
    data_root: String,
    tags: Vec<DataTag>,
}

#[derive(Deserialize, Serialize, Debug)]
struct DataTag {
    name: String,
    value: String,
}

#[derive(Serialize, Debug)]
struct DataItemHeader {
    id: String,
    offset: usize,
}

#[derive(Serialize, Debug)]
struct DataItemMetadata {
    signature_type: u16,
    signature: String,      // 512 bytes
    owner: String,          // 32 bytes
    target: Option<String>, // 1 + (32 bytes)
    anchor: Option<String>, // 1 + (32 bytes)
    number_of_tags: u64,
    number_of_tag_bytes: usize,
    tags: Vec<DataTag>,
}


async fn get_tx_header(tx_id: &str) -> Result<TxHeader> {
    let tx_header = get(format!("{}/tx/{}", NODE_URL, tx_id))
        .await?
        .json::<TxHeader>()
        .await?;

    let mut valid_ans_104_format = false;
    let mut valid_ans_104_version = false;
    for tag in &tx_header.tags {
        if tag.name.as_str() == "QnVuZGxlLUZvcm1hdA" && tag.value.as_str() == "YmluYXJ5" {
            valid_ans_104_format = true
        } else if tag.name.as_str() == "QnVuZGxlLVZlcnNpb24" && tag.value.as_str() == "Mi4wLjA" {
            valid_ans_104_version = true
        }
    }

    if valid_ans_104_format && valid_ans_104_version {
        Ok(tx_header)
    } else {
        Err(anyhow!("Could not get headers"))
    }
}

async fn get_size_and_offset(tx_id: &str) -> Result<(u64, u64)> {
    #[derive(Deserialize)]
    struct SizeOffset {
        size: String,
        offset: String,
    }

    let SizeOffset { size, offset } = get(format!("{}/tx/{}/offset", NODE_URL, tx_id))
        .await?
        .json::<SizeOffset>()
        .await?;
    Ok((size.parse::<u64>()?, offset.parse::<u64>()?))
}

async fn get_chunk(size: u64, offset: u64) -> Result<Vec<u8>> {
    let init_offset = offset - size + 1;
    let url = format!("{}/chunk/{}", NODE_URL, init_offset);
    let data = get(url).await?;
    let text = data.text().await?;

    // Can also splice the byte stream for better performance
    lazy_static! {
        static ref RE: Regex = Regex::new("\"chunk\":\\s*\"(.*)\"").expect("Error compiling regex");
    }
    let caps = RE.captures(&text).expect("No captures were found");
    let chunk_text = &caps[1];
    Ok(base64_url::decode(chunk_text)?)
}

fn get_chunk_metadata(chunk_bytes: &[u8]) -> (usize, Vec<DataItemHeader>) {
    let num_data_items = u64::from_le_bytes(chunk_bytes[0..8].try_into().unwrap());
    let chunk_metadata_end = (num_data_items as usize) * 64 + 32;
    let offset_data = &chunk_bytes[32..chunk_metadata_end];

    (
        chunk_metadata_end,
        offset_data
            .chunks(64)
            .map(|slice| DataItemHeader {
                offset: u64::from_le_bytes(slice[0..8].try_into().unwrap()) as usize,
                id: base64_url::encode(&slice[32..64]),
            })
            .collect(),
    )
}

fn get_data_item(
    data_item_id: &str,
    chunk_bytes: &[u8],
    chunk_metadata_end: usize,
    chunk_metadata: Vec<DataItemHeader>,
) -> Result<(DataItemMetadata, usize, usize)> {
    let mut offset = chunk_metadata_end;
    let mut end_byte = None;
    for header in chunk_metadata {
        if header.id == data_item_id {
            end_byte = Some(offset + header.offset);
            break;
        }
        offset += header.offset;
    }
    let end_byte = end_byte.ok_or_else(|| anyhow!("Could not find matching data item ID"))?;

    let signature_type = u16::from_le_bytes(chunk_bytes[offset..offset + 2].try_into()?);
    offset += 2;

    let signature = base64_url::encode(&chunk_bytes[offset..offset + 512]);
    offset += 512;

    let owner = base64_url::encode(&chunk_bytes[offset..offset + 32]);
    offset += 32;

    let target = if chunk_bytes[offset] == 1 {
        offset += 33;
        Some(base64_url::encode(&chunk_bytes[offset - 32..offset]))
    } else {
        offset += 1;
        None
    };

    let anchor = if chunk_bytes[offset] == 1 {
        offset += 33;
        Some(base64_url::encode(&chunk_bytes[offset - 32..offset]))
    } else {
        offset += 1;
        None
    };

    let number_of_tags = u64::from_le_bytes(chunk_bytes[offset..offset+64].try_into()?);
    offset += 64;

    let number_of_tag_bytes = usize::from_le_bytes(chunk_bytes[offset..offset+64].try_into()?);
    offset += 64;

    let tags_string = std::str::from_utf8(&chunk_bytes[offset..offset+number_of_tag_bytes as usize])?;
    let tags = serde_json::from_str::<Vec<DataTag>>(tags_string)?;
    offset += number_of_tag_bytes;

    let data_item_metadata = DataItemMetadata {
        signature_type,
        signature,
        owner,
        target,
        anchor,
        number_of_tags,
        number_of_tag_bytes,
        tags
    };

    Ok((data_item_metadata, offset, end_byte))
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let params = &std::env::args().collect::<Vec<String>>();
    let bundle_id = params[1].as_str();
    // let bundle_id = "3XEnfj9dmfGxCw6Bfl1LWGmaj12pR6laPFwNbU5C1Nw";

    //let data_item_id = params[2].as_str();
    let data_item_id = "tyKLk8wbY5-rnfa8_fUGuY3IalSBN4haGUZcel-gukw";

    // let tx_header = get_tx_header(&bundle_id).await?;
    let (size, offset) = get_size_and_offset(bundle_id).await?;
    let chunk = get_chunk(size, offset).await?;
    let (chunk_metadata_end, chunk_metadata) = get_chunk_metadata(&chunk);
    let (data_item_metadata, _, _) = get_data_item(data_item_id, chunk.as_slice(), chunk_metadata_end, chunk_metadata)?;

    let metadata_string = serde_json::to_string(&data_item_metadata)?;
    fs::write("data_item_metadata.json", metadata_string)?;


    let stream = TcpStream::connect("127.0.0.1:8000").await?;
    let mut buf = [0; 10];
    let mut buf = ReadBuf::new(&mut buf);

    // let mut buf = vec![0; 128];

    // loop {
    //     let n = rd.read(&mut buf).await?;

    //     if n == 0 {
    //         break;
    //     }

    //     println!("GOT {:?}", &buf[..n]);
    // }

    Ok(())
}
