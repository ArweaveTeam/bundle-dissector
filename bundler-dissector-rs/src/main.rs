use anyhow::{anyhow, Result};
use reqwest::get;
use serde::Deserialize;

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

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let id = "h_q6B9f2zH3lqlq4DhLqDrnrKnNAcFQKvwxG7zCFyTw"; // TODO change to cli param

    let tx_header = get_tx_header(&id).await?;
    let size_offset = get_size_and_offset(id).await?;
    dbg!(tx_header, size_offset);
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
