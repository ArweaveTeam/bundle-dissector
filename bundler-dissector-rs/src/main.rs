#[macro_use]
extern crate lazy_static;

use anyhow::{anyhow, Result};
use regex::Regex;
use reqwest::get;
use serde::{Deserialize, Serialize};
use std::{convert::TryInto, fs};

mod stitcher;

#[derive(Deserialize, Debug)]
struct BundleHeader {
    data_root: String,
    tags: Vec<DataTag>,
}

#[derive(Deserialize, Serialize, Debug)]
struct DataTag {
    name: String,
    value: String,
}

#[derive(Serialize, Debug)]
struct DataItemOffset {
    id: String,
    offset: usize,
}

#[derive(Serialize, Debug)]
struct DataItemHeader {
    signature_type: u16,
    signature: String,      // 512 bytes
    owner: String,          // 32 bytes 
    target: Option<String>, // 1 + (32 bytes)
    anchor: Option<String>, // 1 + (32 bytes)
    number_of_tags: u64,
    number_of_tag_bytes: usize,
    tags: Vec<DataTag>,
}

#[tokio::main(flavor = "current_thread")]
async fn main() -> Result<()> {
    let params = &std::env::args().collect::<Vec<String>>();
    let bundle_id = params[1].as_str();
    // let bundle_id = "3XEnfj9dmfGxCw6Bfl1LWGmaj12pR6laPFwNbU5C1Nw";
    // let data_item_id = params[2].as_str();
    let data_item_id = "tyKLk8wbY5-rnfa8_fUGuY3IalSBN4haGUZcel-gukw";

    // let tx_header = get_bundle_header(&bundle_id).await?;
    let (size, offset) = get_size_and_offset(bundle_id).await?;
    let chunk = get_chunk_by_size_and_end(size, offset).await?;
    let (bundle_metadata_end, bundle_metadata) = get_bundle_metadata(&chunk);
    let (data_item_metadata, _, _) = get_data_item(data_item_id, chunk.as_slice(), bundle_metadata_end, bundle_metadata)?;

    let metadata_string = serde_json::to_string(&data_item_metadata)?;
    fs::write("data_item_metadata.json", metadata_string)?;

    Ok(())
}



async fn get_chunk_by_size_and_end(size: u64, chunk_end: u64) -> Result<Vec<u8>> {
    let init_chunk_end = chunk_end - size + 1;
    Ok(stitcher::get_chunk(init_chunk_end).await?)
}

async fn get_bundle_header(tx_id: &str) -> Result<BundleHeader> {
    let tx_header = get(format!("{}/tx/{}", stitcher::NODE_URL, tx_id))
        .await?
        .json::<BundleHeader>()
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

    let SizeOffset { size, offset } = get(format!("{}/tx/{}/offset", stitcher::NODE_URL, tx_id))
        .await?
        .json::<SizeOffset>()
        .await?;
    Ok((size.parse::<u64>()?, offset.parse::<u64>()?))
}



fn get_bundle_metadata(chunk_bytes: &[u8]) -> (usize, Vec<DataItemOffset>) {
    let num_data_items = u64::from_le_bytes(chunk_bytes[0..8].try_into().unwrap());
    let bundle_metadata_end = (num_data_items as usize) * 64 + 32;
    let offset_data = &chunk_bytes[32..bundle_metadata_end];

    (
        bundle_metadata_end,
        offset_data
            .chunks(64)
            .map(|slice| DataItemOffset {
                offset: u64::from_le_bytes(slice[0..8].try_into().unwrap()) as usize,
                id: base64_url::encode(&slice[32..64]),
            })
            .collect(),
    )
}

fn get_data_item(
    data_item_id: &str,
    chunk_bytes: &[u8],
    bundle_metadata_end: usize,
    bundle_metadata: Vec<DataItemOffset>,
) -> Result<(DataItemHeader, usize, usize)> {
    let mut offset = bundle_metadata_end;
    let mut end_byte = None;
    for header in bundle_metadata {
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

    let data_item_metadata = DataItemHeader {
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