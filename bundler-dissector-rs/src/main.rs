#[macro_use]
extern crate lazy_static;

use std::fs;
use anyhow::{anyhow, Result};
use num_bigint::BigUint;
use regex::Regex;
use reqwest::get;
use serde::{Deserialize, Serialize};
const NODE_URL: &str = "http://gateway-2-temp.arweave.net:1984";

#[derive(Deserialize, Debug)]
struct TxHeader {
    data_root: String,
    tags: Vec<TxHeaderTag>,
}

#[derive(Deserialize, Debug)]
struct TxHeaderTag {
    name: String,
    value: String,
}

#[derive(Serialize, Debug)]
struct DataItemMetadata {
    id: String,
    offset: usize,
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let param = &std::env::args().collect::<Vec<String>>()[0];
    let id = param.as_str();
    // let id = "IgVQ9ux-L-iZhbGHj2RpwyoSt1vIOW2LlKLGiQv2oDM";
    // let id = "h_q6B9f2zH3lqlq4DhLqDrnrKnNAcFQKvwxG7zCFyTw";

    // let tx_header = get_tx_header(&id).await?;
    let (size, offset) = get_size_and_offset(id).await?;
    let chunk = get_chunk(size, offset).await?;
    let (_, metadata) = get_data_item_metadata(&chunk);

    let metadata_string = serde_json::to_string(&metadata)?;
    fs::write("metadata.json", metadata_string)?;

    Ok(())
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

fn get_data_item_metadata(chunk_bytes: &[u8]) -> (usize, Vec<DataItemMetadata>) {
    let num_data_items = BigUint::from_bytes_le(&chunk_bytes[0..32]).to_u64_digits()[0];
    let metadata_end_byte = (num_data_items as usize) * 64 + 32;
    let offset_data = &chunk_bytes[32..metadata_end_byte];
    
    (metadata_end_byte, offset_data
        .chunks(64)
        .map(|slice| DataItemMetadata {
            offset: BigUint::from_bytes_le(&slice[0..32]).to_u64_digits()[0] as usize,
            id: base64_url::encode(&slice[32..64]),
        })
        .collect())
}
