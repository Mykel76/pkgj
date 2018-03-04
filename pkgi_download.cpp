extern "C" {
#include "pkgi.h"
#include "pkgi_dialog.h"
#include "pkgi_utils.h"
}
#include "pkgi_download.hpp"

#include <stddef.h>

// clang-format off
static const uint8_t pkg_psp_key[] = { 0x07, 0xf2, 0xc6, 0x82, 0x90, 0xb5, 0x0d, 0x2c, 0x33, 0x81, 0x8d, 0x70, 0x9b, 0x60, 0xe6, 0x2b };
static const uint8_t pkg_vita_2[] = { 0xe3, 0x1a, 0x70, 0xc9, 0xce, 0x1d, 0xd7, 0x2b, 0xf3, 0xc0, 0x62, 0x29, 0x63, 0xf2, 0xec, 0xcb };
static const uint8_t pkg_vita_3[] = { 0x42, 0x3a, 0xca, 0x3a, 0x2b, 0xd5, 0x64, 0x9f, 0x96, 0x86, 0xab, 0xad, 0x6f, 0xd8, 0x80, 0x1f };
static const uint8_t pkg_vita_4[] = { 0xaf, 0x07, 0xfd, 0x59, 0x65, 0x25, 0x27, 0xba, 0xf1, 0x33, 0x89, 0x66, 0x8b, 0x17, 0xd9, 0xea };
// clang-format on

void Download::download_start(void)
{
    LOG("resuming pkg download from %llu offset", download_offset);
    download_resume = 0;
    info_update = pkgi_time_msec() + 1000;
    pkgi_dialog_set_progress_title("Downloading");
}

int Download::download_data(
        uint8_t* buffer, uint32_t size, int encrypted, int save)
{
    if (pkgi_dialog_is_cancelled())
    {
        pkgi_save(resume_file, &sha, sizeof(sha));
        return 0;
    }

    update_progress(*this);

    if (download_resume)
    {
        int read = pkgi_read(item_file, buffer, size);
        if (read < 0)
        {
            char error[256];
            pkgi_snprintf(
                    error, sizeof(error), "failed to read file %s", item_path);
            pkgi_dialog_error(error);
            return -1;
        }

        if (read != 0)
        {
            // this is only for non-encrypted files (head/tail) when resuming
            download_offset += read;
            return read;
        }

        // not more data to read, need to start actual download
        download_start();
    }

    if (!http)
    {
        initial_offset = download_offset;
        LOG("requesting %s @ %llu", download_url, download_offset);
        http = pkgi_http_get(download_url, download_content, download_offset);
        if (!http)
        {
            pkgi_dialog_error("cannot send HTTP request");
            return 0;
        }

        int64_t http_length;
        if (!pkgi_http_response_length(http, &http_length))
        {
            pkgi_dialog_error("HTTP request failed");
            return 0;
        }
        if (http_length < 0)
        {
            pkgi_dialog_error("HTTP response has unknown length");
            return 0;
        }

        download_size = http_length + download_offset;

        if (!pkgi_check_free_space(http_length))
        {
            return 0;
        }

        LOG("http response length = %lld, total pkg size = %llu",
            http_length,
            download_size);
        info_start = pkgi_time_msec();
        info_update = pkgi_time_msec() + 500;
    }

    int read = pkgi_http_read(http, buffer, size);
    if (read < 0)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "HTTP download error 0x%08x", read);
        pkgi_dialog_error(error);
        pkgi_save(resume_file, &sha, sizeof(sha));
        return -1;
    }
    else if (read == 0)
    {
        pkgi_dialog_error("HTTP connection closed");
        pkgi_save(resume_file, &sha, sizeof(sha));
        return -1;
    }
    download_offset += read;

    sha256_update(&sha, buffer, read);

    if (encrypted)
    {
        aes128_ctr(&aes, iv, encrypted_base + encrypted_offset, buffer, read);
        encrypted_offset += read;
    }

    if (save)
    {
        uint32_t write;
        if (encrypted)
        {
            write = (uint32_t)min64(decrypted_size, read);
            decrypted_size -= write;
        }
        else
        {
            write = read;
        }

        if (!pkgi_write(item_file, buffer, write))
        {
            char error[256];
            pkgi_snprintf(
                    error, sizeof(error), "failed to write to %s", item_path);
            pkgi_dialog_error(error);
            return -1;
        }
    }

    return read;
}

// this includes creating of all the parent folders necessary to actually
// create file
int Download::create_file(void)
{
    char folder[256];
    pkgi_strncpy(folder, sizeof(folder), item_path);
    char* last = pkgi_strrchr(folder, '/');
    *last = 0;

    if (!pkgi_mkdirs(folder))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot create folder %s", folder);
        pkgi_dialog_error(error);
        return 1;
    }

    LOG("creating %s file", item_name);
    item_file = pkgi_create(item_path);
    if (!item_file)
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot create file %s", item_name);
        pkgi_dialog_error(error);
        return 0;
    }

    return 1;
}

int Download::download_head(const uint8_t* rif)
{
    LOG("downloading pkg head");

    pkgi_strncpy(item_name, sizeof(item_name), "Preparing...");
    pkgi_snprintf(
            item_path, sizeof(item_path), "%s/sce_sys/package/head.bin", root);

    int result = 0;

    if (download_resume)
    {
        item_file = pkgi_openrw(item_path);
        if (item_file)
        {
            LOG("trying to resume %s file", item_name);
        }
        else
        {
            download_start();
        }
    }

    if (!download_resume)
    {
        if (!create_file())
        {
            char error[256];
            pkgi_snprintf(error, sizeof(error), "cannot create %s", item_path);
            pkgi_dialog_error(error);
            goto bail;
        }
    }

    head_size = PKG_HEADER_SIZE + PKG_HEADER_EXT_SIZE;
    uint32_t head_offset = 0;
    while (head_offset != head_size)
    {
        int size = download_data(
                head + head_offset, head_size - head_offset, 0, 1);
        if (size <= 0)
        {
            goto bail;
        }
        head_offset += size;
    }

    if (get32be(head) != 0x7f504b47 ||
        get32be(head + PKG_HEADER_SIZE) != 0x7F657874)
    {
        pkgi_dialog_error("wrong pkg header");
        goto bail;
    }

    if (rif && !pkgi_memequ(rif + 0x10, head + 0x30, 0x30))
    {
        pkgi_dialog_error("zRIF content id doesn't match pkg");
        goto bail;
    }

    meta_offset = get32be(head + 8);
    meta_count = get32be(head + 12);
    index_count = get32be(head + 20);
    total_size = get64be(head + 24);
    enc_offset = get64be(head + 32);
    enc_size = get64be(head + 40);
    LOG("meta_offset=%u meta_count=%u index_count=%u total_size=%llu "
        "enc_offset=%llu enc_size=%llu",
        meta_offset,
        meta_count,
        index_count,
        total_size,
        enc_offset,
        enc_size);

    if (enc_offset > sizeof(head))
    {
        LOG("pkg file head is too large");
        pkgi_dialog_error("pkg is not supported, head.bin is too big");
        goto bail;
    }

    pkgi_memcpy(iv, head + 0x70, sizeof(iv));

    uint8_t key[AES_BLOCK_SIZE];
    int key_type = head[0xe7] & 7;
    if (key_type == 1)
    {
        pkgi_memcpy(key, pkg_psp_key, sizeof(key));
    }
    else if (key_type == 2)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_2);
        aes128_encrypt(&ctx, iv, key);
    }
    else if (key_type == 3)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_3);
        aes128_encrypt(&ctx, iv, key);
    }
    else if (key_type == 4)
    {
        aes128_ctx ctx;
        aes128_init(&ctx, pkg_vita_4);
        aes128_encrypt(&ctx, iv, key);
    }

    aes128_ctr_init(&aes, key);

    uint32_t target_size = (uint32_t)enc_offset;
    while (head_size != target_size)
    {
        int size =
                download_data(head + head_size, target_size - head_size, 0, 1);
        if (size <= 0)
        {
            goto bail;
        }
        head_size += size;
    }

    index_size = 0;

    uint32_t index_offset = 1;
    uint32_t offset = meta_offset;
    for (uint32_t i = 0; i < meta_count; i++)
    {
        if (offset + 16 >= enc_offset)
        {
            pkgi_dialog_error("pkg file is too small or corrupted");
            goto bail;
        }

        uint32_t type = get32be(head + offset + 0);
        uint32_t size = get32be(head + offset + 4);

        if (type == 2)
        {
            uint32_t content_type = get32be(head + offset + 8);
            if (content_type != 21)
            {
                pkgi_dialog_error("pkg is not a main package");
                goto bail;
            }
        }
        else if (type == 13)
        {
            index_offset = get32be(head + offset + 8);
            index_size = get32be(head + offset + 12);
        }
        offset += 8 + size;
    }

    if (index_offset != 0 || index_size == 0)
    {
        pkgi_dialog_error("pkg is missing encrypted file index");
        goto bail;
    }

    target_size = (uint32_t)(enc_offset + index_size);
    if (target_size > sizeof(head))
    {
        LOG("pkg file head is too large");
        pkgi_dialog_error("pkg is not supported, head.bin is too big");
        goto bail;
    }

    while (head_size != target_size)
    {
        int size =
                download_data(head + head_size, target_size - head_size, 0, 1);
        if (size <= 0)
        {
            goto bail;
        }
        head_size += size;
    }

    LOG("head.bin downloaded");
    result = 1;

bail:
    if (item_file)
    {
        pkgi_close(item_file);
        item_file = NULL;
    }

    return result;
}

int Download::download_files(void)
{
    LOG("downloading encrypted files");

    int result = 0;

    for (uint32_t index = 0; index < index_count; index++)
    {
        uint8_t item[32];
        pkgi_memcpy(
                item, head + enc_offset + sizeof(item) * index, sizeof(item));
        aes128_ctr(&aes, iv, sizeof(item) * index, item, sizeof(item));

        uint32_t name_offset = get32be(item + 0);
        uint32_t name_size = get32be(item + 4);
        uint64_t item_offset = get64be(item + 8);
        uint64_t item_size = get64be(item + 16);
        uint8_t type = item[27];

        if (name_size > sizeof(item_name) - 1 ||
            enc_offset + name_offset + name_size > total_size)
        {
            pkgi_dialog_error("pkg file is too small or corrupted");
            goto bail;
        }

        pkgi_memcpy(item_name, head + enc_offset + name_offset, name_size);
        aes128_ctr(&aes, iv, name_offset, (uint8_t*)item_name, name_size);
        item_name[name_size] = 0;

        LOG("[%u/%u] %s item_offset=%llu item_size=%llu type=%u",
            item_index + 1,
            index_count,
            item_name,
            item_offset,
            item_size,
            type);

        if (type == 4 || type == 18)
        {
            continue;
        }

        pkgi_snprintf(item_path, sizeof(item_path), "%s/%s", root, item_name);

        uint64_t encrypted_size = (item_size + AES_BLOCK_SIZE - 1) &
                                  ~((uint64_t)AES_BLOCK_SIZE - 1);
        decrypted_size = item_size;
        encrypted_base = item_offset;
        encrypted_offset = 0;
        item_index = index;

        if (download_resume)
        {
            if (pkgi_dialog_is_cancelled())
            {
                goto bail;
            }

            int64_t current_size = pkgi_get_size(item_path);
            if (current_size < 0)
            {
                LOG("file does not exist %s", item_path);
                download_start();
            }
            else if ((uint64_t)current_size != decrypted_size)
            {
                LOG("downloaded %llu, total %llu, resuming %s",
                    (uint64_t)current_size,
                    decrypted_size,
                    item_path);
                item_file = pkgi_append(item_path);
                if (!item_file)
                {
                    char error[256];
                    pkgi_snprintf(
                            error,
                            sizeof(error),
                            "cannot append to %s",
                            item_name);
                    pkgi_dialog_error(error);
                    goto bail;
                }
                encrypted_offset = (uint64_t)current_size;
                decrypted_size -= current_size;
                download_offset += current_size;
                download_start();
            }
            else
            {
                LOG("file fully downloaded %s", item_name);
                download_offset += encrypted_size;
                update_progress(*this);
                continue;
            }
        }

        // if we are starting to download file from scratch
        if (!download_resume && !item_file)
        {
            if (!create_file())
            {
                char error[256];
                pkgi_snprintf(
                        error, sizeof(error), "cannot create %s", item_name);
                pkgi_dialog_error(error);
                goto bail;
            }
        }

        if (enc_offset + item_offset + encrypted_offset != download_offset)
        {
            pkgi_dialog_error("pkg is not supported, files are in wrong order");
            goto bail;
        }

        if (enc_offset + item_offset + item_size > total_size)
        {
            pkgi_dialog_error("pkg file is too small or corrupted");
            goto bail;
        }

        while (encrypted_offset != encrypted_size)
        {
            uint32_t read = (uint32_t)min64(
                    sizeof(down), encrypted_size - encrypted_offset);
            int size = download_data(down, read, 1, 1);
            if (size <= 0)
            {
                goto bail;
            }
        }

        pkgi_close(item_file);
        item_file = NULL;
    }

    item_index = -1;

    LOG("all files decrypted");
    result = 1;

bail:
    if (item_file)
    {
        pkgi_close(item_file);
        item_file = NULL;
    }
    return result;
}

int Download::download_tail(void)
{
    LOG("downloading tail.bin");

    int result = 0;

    pkgi_strncpy(item_name, sizeof(item_name), "Finishing...");
    pkgi_snprintf(
            item_path, sizeof(item_path), "%s/sce_sys/package/tail.bin", root);

    if (download_resume)
    {
        download_start();
    }

    if (!create_file())
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot create %s", item_path);
        pkgi_dialog_error(error);
        goto bail;
    }

    uint64_t tail_offset = enc_offset + enc_size;
    while (download_offset < tail_offset)
    {
        uint32_t read =
                (uint32_t)min64(sizeof(down), tail_offset - download_offset);
        int size = download_data(down, read, 0, 0);
        if (size <= 0)
        {
            goto bail;
        }
    }

    while (download_offset != total_size)
    {
        uint32_t read =
                (uint32_t)min64(sizeof(down), total_size - download_offset);
        int size = download_data(down, read, 0, 1);
        if (size <= 0)
        {
            goto bail;
        }
    }

    LOG("tail.bin downloaded");
    result = 1;

bail:
    if (item_file != NULL)
    {
        pkgi_close(item_file);
        item_file = NULL;
    }
    return result;
}

int Download::check_integrity(const uint8_t* digest)
{
    if (!digest)
    {
        LOG("no integrity provided, skipping check");
        return 1;
    }

    uint8_t check[SHA256_DIGEST_SIZE];
    sha256_finish(&sha, check);

    LOG("checking integrity of pkg");
    if (!pkgi_memequ(digest, check, SHA256_DIGEST_SIZE))
    {
        LOG("pkg integrity is wrong, removing head.bin & resume data");

        char path[256];
        pkgi_snprintf(path, sizeof(path), "%s/sce_sys/package/head.bin", root);
        pkgi_rm(path);

        pkgi_dialog_error("pkg integrity failed, try downloading again");
        return 0;
    }

    LOG("pkg integrity check succeeded");
    return 1;
}

int Download::create_rif(const uint8_t* rif)
{
    LOG("creating work.bin");
    pkgi_dialog_update_progress("Creating work.bin", NULL, NULL, 1.f);

    char path[256];
    pkgi_snprintf(path, sizeof(path), "%s/sce_sys/package/work.bin", root);

    if (!pkgi_save(path, rif, PKGI_RIF_SIZE))
    {
        char error[256];
        pkgi_snprintf(error, sizeof(error), "cannot save rif to %s", path);
        pkgi_dialog_error(error);
        return 0;
    }

    LOG("work.bin created");
    return 1;
}

int Download::pkgi_download(
        const char* content,
        const char* url,
        const uint8_t* rif,
        const uint8_t* digest)
{
    int result = 0;

    pkgi_snprintf(
            root, sizeof(root), "%s/%.9s", pkgi_get_temp_folder(), content + 7);
    LOG("temp installation folder: %s", root);

    pkgi_snprintf(
            resume_file,
            sizeof(resume_file),
            "%s/%.9s.resume",
            pkgi_get_temp_folder(),
            content + 7);
    if (pkgi_load(resume_file, &sha, sizeof(sha)) == sizeof(sha))
    {
        LOG("resume file exists, trying to resume");
        pkgi_dialog_set_progress_title("Resuming");
        download_resume = 1;
    }
    else
    {
        LOG("cannot load resume file, starting download from scratch");
        pkgi_dialog_set_progress_title("Downloading");
        download_resume = 0;
        sha256_init(&sha);
    }

    http = NULL;
    item_file = NULL;
    item_index = -1;
    download_size = 0;
    download_offset = 0;
    download_content = content;
    download_url = url;

    info_start = pkgi_time_msec();
    info_update = info_start + 1000;

    if (!download_head(rif))
        goto finish;
    if (!download_files())
        goto finish;
    if (!download_tail())
        goto finish;
    if (!check_integrity(digest))
        goto finish;
    if (rif)
    {
        if (!create_rif(rif))
            goto finish;
    }

    pkgi_rm(resume_file);
    result = 1;

finish:
    if (http)
    {
        pkgi_http_close(http);
    }

    return result;
}
