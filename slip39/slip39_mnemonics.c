#include "slip39.h"

//////////////////////////////////////////////////
// encode mnemonic
unsigned int encode_mnemonic(
    const slip39_share *share,
    uint16_t *destination,
    uint32_t destination_length) {

    // pack the id, exp, group and member data into 4 10-bit words:
    // [id:1  5][exp:5][g_index:4][g_thresh*:4][g_count*:4][m_idx:4][m_thrsh*:4]s
    // [w0:10][  w1:10][w2:10                      ][w3:10                     ]

    // change offset and clip group and member coordinate data
    uint16_t gt = (share->group_threshold -1) & 15;
    uint16_t gc = (share->group_count -1) & 15;
    uint16_t mi = (share->member_index) & 15;
    uint16_t mt = (share->member_threshold -1) & 15;

    destination[0] = (share->identifier >> 5) & 1023;
    destination[1] = ((share->identifier << 5) | share->iteration_exponent) & 1023;
    destination[2] = ((share->group_index << 6) | (gt << 2) | (gc >> 2)) & 1023;
    destination[3] = ((gc << 8) | (mi << 4) | (mt)) & 1023;

    uint32_t words = toWords(share->value, share->value_length, destination+4, destination_length - METADATA_LENGTH_WORDS);
    rs1024_create_checksum(destination, words + METADATA_LENGTH_WORDS);

    return words+METADATA_LENGTH_WORDS;
 }

//////////////////////////////////////////////////
// decode mnemonic
unsigned int decode_mnemonic(
    const uint16_t *mnemonic,
    uint32_t mnemonic_length,
    slip39_share *share
) {
    if(mnemonic_length < MIN_MNEMONIC_LENGTH_WORDS) {
        return ERROR_NOT_ENOUGH_MNEMONIC_WORDS;
    }

    if( !rs1024_verify_checksum(mnemonic, mnemonic_length) ) {
        return ERROR_INVALID_MNEMONIC_CHECKSUM;
    }

    uint8_t gt = ((mnemonic[2] >> 2) & 15) +1;
    uint8_t gc = (((mnemonic[2]&3) << 2) | ((mnemonic[3]>>8)&3)) +1;

    if(gt > gc) {
        return ERROR_INVALID_MNEMONIC_GROUP_THRESHOLD;
    }

    share->identifier = mnemonic[0] << 5 | mnemonic[1] >> 5;
    share->iteration_exponent = mnemonic[1] & 31;
    share->group_index = mnemonic[2] >> 6;
    share->group_threshold = gt;
    share->group_count = gc;
    share->member_index = (mnemonic[3]>>4) & 15;
    share->member_threshold = (mnemonic[3]&15) + 1;
    share->value_length=fromWords(mnemonic+4, mnemonic_length - 7, share->value, share->value_length);
    if(share->value_length < MIN_STRENGTH_BYTES) {
        return ERROR_SECRET_TOO_SHORT;
    }
    if(share->value_length % 2) {
        return ERROR_INVALID_SECRET_LENGTH;
    }
    return share->value_length;
}


void print_hex(
    const uint8_t *buffer,
    uint32_t length
) {
    printf("0x");
    for(uint32_t i=0;i<length;++i) {
        if(i > 0 && i%32== 0) {
            printf("\n  ");
        }
        printf("%02x", buffer[i]);
    }
    printf("\n");
}


void print_mnemonic(
    const uint16_t *mnemonic,
    unsigned int mnemonic_length
) {
    uint8_t value[256];

    slip39_share share;
    share.value = value;
    share.value_length = 256;

    unsigned int secret_length = decode_mnemonic(mnemonic, mnemonic_length, &share);
    share.value_length = secret_length;

    for(unsigned int i=0;i< mnemonic_length; ++i) {
        printf("%s ", slip39_word(mnemonic[i]));
    }

    printf("\n");
    printf("identifier: %d  exponent: %d\n", share.identifier, share.iteration_exponent);
    printf("group index: %d  threshold: %d  count: %d\n",
        share.group_index, share.group_threshold, share.group_count);
    printf("member index: %d  threshold: %d\n",
        share.member_index, share.member_threshold);
    print_hex(share.value, share.value_length);
}

//////////////////////////////////////////////////
// generate mnemonics
//
int generate_mnemonics(
    uint8_t group_threshold,
    const group_descriptor *groups,
    uint8_t groups_length,
    const uint8_t *master_secret,
    uint32_t master_secret_length,
    const char *passphrase,
    uint8_t iteration_exponent,
    uint32_t *mnemonic_length,
    uint16_t *mnemonics,
    uint32_t buffer_size
) {

    if(master_secret_length < MIN_STRENGTH_BYTES) {
        return ERROR_SECRET_TOO_SHORT;
    }

    // assign a random identifier
    uint16_t identifier = 0;
    randombytes((uint8_t *)(&identifier), 2);
    identifier = identifier & ((1<<15)-1);

    uint32_t share_length = METADATA_LENGTH_WORDS + bytes_to_words(master_secret_length);
    uint32_t total_shares = 0;

    for(uint8_t i=0; i<groups_length; ++i) {
        total_shares += groups[i].count;
        if( groups[i].threshold > groups[i].count ) {
            return ERROR_INVALID_GROUP_THRESHOLD;
        }
        if( groups[i].threshold == 1 && groups[i].count > 1) {
            return ERROR_INVALID_SINGLETOM_MEMBER;
        }
    }

    if(buffer_size < share_length * total_shares) {
        return ERROR_INSUFFICIENT_SPACE;
    }

    if(master_secret_length % 2 == 1) {
        return ERROR_INVALID_SECRET_LENGTH;
    }

    for(const uint8_t *p = (const uint8_t *) passphrase; *p; p++) {
        if( (*p < 32) || (126 < *p) ) {
            return ERROR_INVALID_PASSPHRASE;
        }
    }

    if(group_threshold > groups_length) {
        return ERROR_INVALID_GROUP_THRESHOLD;
    }

    uint8_t encrypted_master_secret[master_secret_length];

    slip39_encrypt(master_secret,master_secret_length,passphrase,iteration_exponent,identifier, encrypted_master_secret);

    uint8_t group_shares[master_secret_length * groups_length];

    split_secret(group_threshold, groups_length, encrypted_master_secret, master_secret_length, group_shares);

    uint8_t *group_share = group_shares;

    uint16_t *mnemonic = mnemonics;
    unsigned int remaining_buffer = buffer_size;

     unsigned int word_count = 0;
     unsigned int share_count = 0;

    slip39_share share;
    share.identifier = identifier;
    share.iteration_exponent = iteration_exponent;
    share.group_threshold = group_threshold;
    share.group_count = groups_length;
    share.value_length = master_secret_length;

    for(uint8_t i=0; i<groups_length; ++i, group_share += master_secret_length) {
        uint8_t member_shares[master_secret_length * groups[i].count ];
        split_secret(groups[i].threshold, groups[i].count, group_share, master_secret_length, member_shares);
        share.group_index = i;
        share.member_threshold = groups[i].threshold;

        uint8_t *value = member_shares;
        for(uint8_t j=0; j< groups[i].count; ++j, value += master_secret_length) {
            share.member_index = j;
            share.value = value;

            if(groups[i].passwords && groups[i].passwords[j]) {
                encrypt_share(&share, groups[i].passwords[j]);
            }
            
            unsigned int words = encode_mnemonic(&share, mnemonic, remaining_buffer);

            if(word_count == 0) {
                word_count = words;
            } else {
                if(word_count != words) {
                    memset(member_shares, 0, sizeof(member_shares));
                    memset(encrypted_master_secret, 0, sizeof(encrypted_master_secret));
                    memset(group_shares, 0, sizeof(group_shares));
                    return ERROR_INVALID_SHARE_SET;
                }
            }
            remaining_buffer -= word_count;
            share_count++;
            mnemonic += word_count;

        }

        memset(member_shares, 0, sizeof(member_shares));
        
    }

    memset(encrypted_master_secret, 0, sizeof(encrypted_master_secret));
    memset(group_shares, 0, sizeof(group_shares));

    // store the number of words in each share
    *mnemonic_length = word_count;

    // return the number of shares generated
    return share_count;
}


void print_group(slip39_group *g, unsigned int secret_length) {
    printf("group index: %d  threshold: %d  count: %d\n",
        g->group_index, g->member_threshold, g->count );
    for(uint8_t i=0; i<g->count; ++i) {
        printf("%d: ", g->member_index[i]);
        print_hex(g->value[i], secret_length);
    }
}


/////////////////////////////////////////////////
// combine_mnemonics
int combine_mnemonics(
    const uint16_t **mnemonics, // array of pointers to 10-bit words
    uint32_t mnemonics_words,   // number of words in each share
    uint32_t mnemonics_shares,  // total number of shares
    const char *passphrase,     // passphrase to unlock master secret
    const char **passwords,     // passwords for the shares
    uint8_t *buffer,            // working space, and place to return secret
    uint32_t buffer_length      // total amount of working space
) {
    uint16_t identifier;
    uint8_t iteration_exponent;
    uint8_t group_threshold;
    uint8_t group_count;

    if(mnemonics_shares == 0) {
        return ERROR_EMPTY_MNEMONIC_SET;
    }

    uint8_t next_group = 0;
    slip39_group groups[16];

    // allocate enough space on the stack to reconstruct the member shares
    uint8_t workspace[ (mnemonics_words * mnemonics_shares * 5) / 4 ];
    uint8_t *next_share = workspace;
    uint32_t buffer_remaining = (mnemonics_words * mnemonics_shares * 5) / 4;
    uint32_t secret_length = 0;

    for(unsigned int i=0; i<mnemonics_shares; ++i) {
        slip39_share share;
        share.value = next_share;
        share.value_length = buffer_remaining;

        int32_t bytes = decode_mnemonic(mnemonics[i], mnemonics_words, &share);

        if(bytes < 0) {
            // pass the error code on
            return bytes;
        }

        if(passwords && passwords[i]) {
            decrypt_share(&share, passwords[i]);
        }
        
        // advance pointers into free buffer
        buffer_remaining -= bytes;
        secret_length = bytes;
        next_share += bytes;

        if( i == 0) {
            // on the first one, establish expected values for common metadata
            identifier = share.identifier;
            iteration_exponent = share.iteration_exponent;
            group_count = share.group_count;
            group_threshold = share.group_threshold;
        } else {
            // on subsequent shares, check that common metadata matches
            if( share.identifier != identifier ||
                share.iteration_exponent != iteration_exponent ||
                share.group_threshold != group_threshold ||
                share.group_count != group_count
            ) {
                return ERROR_INVALID_SHARE_SET;
            }
        }

        // sort shares into member groups
        uint8_t group_found = 0;
        for(uint8_t j=0; j<next_group; ++j) {
            if(share.group_index == groups[j].group_index) {
                group_found = 1;
                if(share.member_threshold != groups[j].member_threshold) {
                    return ERROR_INVALID_MEMBER_THRESHOLD;
                }
                for(uint8_t k=0; k<groups[j].count; ++k) {
                    if(share.member_index == groups[j].member_index[k]) {
                        return ERROR_DUPLICATE_MEMBER_INDEX;
                    }
                }
                groups[j].member_index[groups[j].count] = share.member_index;
                groups[j].value[groups[j].count] = share.value;
                groups[j].count++;
            }
        }

        if(!group_found) {
            groups[next_group].group_index = share.group_index;
            groups[next_group].member_threshold = share.member_threshold;
            groups[next_group].count =1;
            groups[next_group].member_index[0] = share.member_index;
            groups[next_group].value[0] = share.value;
            next_group++;
        }
    }


    if(next_group < group_threshold) {
        return ERROR_NOT_ENOUGH_GROUPS;
    }
    
    // here, all of the shares are unpacked into member groups. Now we go through each
    // group and recover the group secret, and then use the result to recover the
    // master secret
    uint8_t gx[16];
    const uint8_t *gy[16];

    // allocate enough space for the group shares and the encrypted master secret
    uint8_t group_shares[secret_length * (group_threshold + 1)];
    next_share = group_shares;

    for(uint8_t i=0; i<next_group; ++i) {
        gx[i] = groups[i].group_index;
        if(groups[i].count < groups[i].member_threshold) {
            return ERROR_NOT_ENOUGH_MEMBER_SHARES;
        }

        int recovery = recover_secret(
            groups[i].member_threshold, groups[i].member_index,
            groups[i].value, secret_length, next_share);

        if(recovery < 0) {
            return recovery;
        }
        gy[i] = next_share;

        next_share += recovery;
    }
    
    int recovery = recover_secret(group_threshold, gx, gy, secret_length, next_share);

    memset(workspace,0,sizeof(workspace));
    
    if(recovery < 0) {
        memset(group_shares,0,sizeof(group_shares));
        return recovery;
    }

    // decrypt copy the result to the beinning of the buffer supplied
    slip39_decrypt(next_share, secret_length, passphrase, iteration_exponent, identifier, buffer);

    memset(group_shares,0,sizeof(group_shares));

    // TODO: clean up scratch memory
    return secret_length;
}


////
// encrypt/decrypt shares
//
void encrypt_share(
    slip39_share *share,
    const char *passphrase
) {
    uint8_t temp[share->value_length];
    slip39_encrypt(share->value, share->value_length, passphrase, share->iteration_exponent, share->identifier, temp);
    memcpy(share->value, temp, share->value_length);        
}

void decrypt_share(
    slip39_share *share,
    const char * passphrase
) {
    uint8_t temp[share->value_length];
    slip39_decrypt(share->value, share->value_length, passphrase, share->iteration_exponent, share->identifier, temp);
    memcpy(share->value, temp, share->value_length);        
}
