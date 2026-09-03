# Attachment storage v1

All Pengy editions share this durable history contract.

* A message may have `attachments`, an ordered array of records with required `v`, `id`, `kind`, `name`, `media_type`, `byte_size`, and `created_at` fields. Image records additionally use `image: {width,height}`.
* IDs are `sha256:` plus 64 lowercase hexadecimal characters.
* Original validated bytes live at `attachments/objects/sha256/<first-two>/<digest>` below Pengy's config directory. Do not store base64/data URLs in chat JSON.
* Image derivatives are `attachments/derivatives/sha256/<first-two>/<digest>/image-display-v1.jpg` and `thumbnail-256-v1.jpg`.
* Source and derivatives are installed with a same-directory temp write followed by rename.
* New readers preserve unknown attachment kinds and fields. Unsupported/missing objects remain references and must not crash rendering or become provider image parts.
* Provider data URLs are transient: derive them only during request assembly. Current turns always resolve; historical attachments resolve only for the most recent `attachment_context_keep_turns` user turns (default 4).
* Legacy `[Image: filename]` strings are plain text and are never inferred as attachment records.
* v1 does not delete objects automatically.
