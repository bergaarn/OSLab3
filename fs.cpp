#include <iostream>
#include <cstring>
#include <vector>
#include <iomanip>
#include <sstream>
#include <string>

#include "fs.h"

FS::FS()
{

}

FS::~FS()
{

}

// Splits a path into parent path + filename
// e.g. "/foo/bar.txt"  -> parent="/foo",  name="bar.txt"
//      "docs/readme"   -> parent="docs",  name="readme"
//      "file.txt"      -> parent="current directory",     name="file.txt"
void
FS::splitParentPath(const std::string& path, std::string& parent, std::string& name) {
    // Find last '/' in the path
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) {
        // No '/' -> parent is current directory
        parent = "";
        name = path;
    } else if (pos == 0) {
        // Path like "/file" -> parent is root
        parent = "/";
        name = path.substr(1);
    } else {
        // General case
        parent = path.substr(0, pos);
        name = path.substr(pos + 1);
    }
}

int 
FS::resolvePath(const std::string& path, bool mustBeDir, uint16_t& outBlock) {
    // 1. Decide starting point
    uint16_t current = (path.size() > 0 && path[0] == '/') ? ROOT_BLOCK : currentDirectory;

    // 2. Split by '/'
    std::stringstream ss(path);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(ss, token, '/')) {
        if (!token.empty()) tokens.push_back(token);
    }

    // Empty path means just "current directory"
    if (tokens.empty()) {
        outBlock = current;
        return 0;
    }

    // 3. Traverse
    for (size_t i = 0; i < tokens.size(); i++) {
        const std::string& part = tokens[i];

        if (part == "..") {
            if (current == ROOT_BLOCK) {
                continue; // stay at root
            }
            uint8_t buf[BLOCK_SIZE];
            disk.read(current, buf);
            dir_entry* entries = reinterpret_cast<dir_entry*>(buf);
            current = entries[0].first_blk; // parent pointer
            continue;
        }

        // Must search current directory for part
        uint8_t buf[BLOCK_SIZE];
        if (disk.read(current, buf) != 0) return -1;
        dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

        bool found = false;
        for (int j = 0; j < BLOCK_SIZE / sizeof(dir_entry); j++) {
            if (strcmp(entries[j].file_name, part.c_str()) == 0) {
                // If it's the last token, maybe check mustBeDir
                if (i == tokens.size() - 1) {
                    if (mustBeDir && entries[j].type != TYPE_DIR) return -2;
                    current = entries[j].first_blk;
                    found = true;
                    break;
                } else {
                    // must be directory if not last
                    if (entries[j].type != TYPE_DIR) return -3;
                    current = entries[j].first_blk;
                    found = true;
                    break;
                }
            }
        }
        if (!found) return -4; // not found
    }

    outBlock = current;
    return 0;
}

// formats the disk, i.e., creates an empty file system
int 
FS::format()
{
    // Set root(0) and FAT(1) slots to used
    fat[ROOT_BLOCK] = FAT_EOF;
    fat[FAT_BLOCK] = FAT_EOF;

    // Mark all blocks as free (except root and FAT)
    const unsigned number_of_blocks = disk.get_no_blocks();
    for (int i = 2; i < number_of_blocks; i++) {
        fat[i] = FAT_FREE;
    }

    // Initialize root directory block
    uint8_t rootBuf[BLOCK_SIZE];
    memset(rootBuf, 0, BLOCK_SIZE);
    dir_entry* rootEntries = reinterpret_cast<dir_entry*>(rootBuf);

    // Add ".." entry that points to root itself
    strcpy(rootEntries[0].file_name, "..");
    rootEntries[0].first_blk = ROOT_BLOCK;  // root points to itself
    rootEntries[0].type = TYPE_DIR;
    rootEntries[0].size = 0;

    // Write root block
    if (disk.write(ROOT_BLOCK, rootBuf) != 0)
        return 1;

    // Write formatted FAT to disk
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 2;

    // Clear all other blocks
    uint8_t emptyBuf[BLOCK_SIZE];
    memset(emptyBuf, 0, BLOCK_SIZE);
    for (int i = 2; i < number_of_blocks; i++) {
        if (disk.write(i, emptyBuf) != 0)
            return 3;
    }

    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int 
FS::create(std::string filepath)
{
    // 1. Split into parent path + file name
    std::string parentPath, name;
    splitParentPath(filepath, parentPath, name);

    // Filename length check
    if (name.size() >= 56) return 1;

    // 2. Resolve parent directory
    uint16_t parentBlock;
    if (resolvePath(parentPath, true, parentBlock) != 0)
        return 2;

    // 3. Read parent directory
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(parentBlock, dirBuffer) != 0)
        return 3;
    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(dirBuffer);

    // 4. Check for duplicate
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (directory_entries[i].file_name[0] != '\0' &&
            strcmp(directory_entries[i].file_name, name.c_str()) == 0) {
            return 4; // already exists
        }
    }

    // 5. Read file content from stdin
    std::string completedText, line;
    while (true) {
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) break;
        completedText.append(line + "\n");
    }

    // 6. Load FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 5;

    // 7. Allocate blocks
    std::vector<uint16_t> freeBlocks;
    int textLeft = completedText.size();
    int textWritten = 0;

    while (textLeft > 0) {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) {
            if (fat[i] == FAT_FREE) { freeBlock = i; break; }
        }
        if (freeBlock == -1) return 6; // no space

        freeBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE] = {};
        int toWrite = std::min(textLeft, (int)BLOCK_SIZE);
        memcpy(buf, completedText.data() + textWritten, toWrite);

        if (disk.write(freeBlock, buf) != 0) return 7;

        textWritten += toWrite;
        textLeft -= toWrite;
    }

    // 8. Update FAT chain
    for (int i = 0; i < (int)freeBlocks.size(); i++) {
        fat[freeBlocks[i]] = (i + 1 < (int)freeBlocks.size())
                           ? freeBlocks[i + 1]
                           : FAT_EOF;
    }
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 8;

    // 9. Create dir_entry
    dir_entry newFile{};
    strncpy(newFile.file_name, name.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.size = completedText.size();
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF : freeBlocks[0];
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE;

    // 10. Insert into parent directory
    bool inserted = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (directory_entries[i].file_name[0] == '\0') {
            directory_entries[i] = newFile;
            inserted = true;
            break;
        }
    }
    if (!inserted) return 9; // directory full

    if (disk.write(parentBlock, dirBuffer) != 0)
        return 10;

    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int
FS::cat(std::string filepath) 
{
    uint16_t block;
    int res = resolvePath(filepath, false, block); // mustBeDir = false
    if (res != 0) return res;

    // Now `block` is the *first block* of the file.
    // But we still need its metadata: size and type.
    // So: we have to re-find the dir_entry for this file.

    // --- Find the parent directory + entry ---
    std::string parentPath, filename;
    splitParentPath(filepath, parentPath, filename); // helper, see below

    uint16_t parentBlock;
    res = resolvePath(parentPath, true, parentBlock);
    if (res != 0) return res;

    uint8_t buf[BLOCK_SIZE];
    disk.read(parentBlock, buf);
    dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

    dir_entry* targetFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (strcmp(entries[i].file_name, filename.c_str()) == 0) {
            targetFile = &entries[i];
            break;
        }
    }
    if (!targetFile) return 2;
    if (targetFile->type == TYPE_DIR) return 3;

    // --- Load FAT ---
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 4;

    // --- Walk file blocks and print ---
    int16_t fileBlock = static_cast<int16_t>(targetFile->first_blk);
    int bytesToRead = targetFile->size;

    while (fileBlock != FAT_EOF && bytesToRead > 0) {
        uint8_t blockBuffer[BLOCK_SIZE];
        if (disk.read(fileBlock, blockBuffer) != 0)
            return 5;

        int bytesToPrint = std::min(bytesToRead, BLOCK_SIZE);
        std::cout.write(reinterpret_cast<char*>(blockBuffer), bytesToPrint);

        bytesToRead -= bytesToPrint;
        fileBlock = fat[fileBlock];
    }

    std::cout << std::endl;
    return 0;
}

// ls lists the content in the currect directory (files and sub-directories)
int
FS::ls()
{
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 1;
    }

    // Leave room for filename
    std::cout 
    //<< std::left << std::setw(56) 
    << "name" 
    //<< std::right 
    << "\t type\t size" << std::endl;
    
    std::string type = "";
    std::string size = "-";
    
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
    {
        if (dir_entries[i].file_name[0] == '\0')
        {
            continue;
        }

        if (strcmp(dir_entries[i].file_name, "..") == 0)
        {
            continue;
        }

        type = (dir_entries[i].type == TYPE_FILE) ? "file" : "dir";
        if (type == "dir")
        {
            std::cout 
            //<< std::left << std::setw(56)
            << dir_entries[i].file_name 
            //<< std::right
            << "\t " << type << "\t "
            << size << std::endl;
        } 
        else
        {
            std::cout
            //<< std::left << std::setw(56)
            << dir_entries[i].file_name 
            //<< std::right
            << "\t " << type << "\t "
            << dir_entries[i].size << std::endl;
        }
        
    }
    //std::cout << std::endl;

    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int 
FS::cp(std::string sourcepath, std::string destpath)
{
    // --- Split source path ---
    std::string srcParent, srcName;
    splitParentPath(sourcepath, srcParent, srcName);

    // --- Resolve source parent directory ---
    uint16_t srcDirBlock;
    if (resolvePath(srcParent, true, srcDirBlock) != 0)
        return 1; // source parent invalid

    // --- Find source file ---
    uint8_t srcDirBuf[BLOCK_SIZE];
    if (disk.read(srcDirBlock, srcDirBuf) != 0)
        return 2;

    dir_entry* srcEntries = reinterpret_cast<dir_entry*>(srcDirBuf);
    dir_entry* srcFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (srcEntries[i].file_name[0] != '\0' &&
            strcmp(srcEntries[i].file_name, srcName.c_str()) == 0) {
            srcFile = &srcEntries[i];
            break;
        }
    }
    if (!srcFile || srcFile->type != TYPE_FILE)
        return 3; // source not found or is a dir

    // --- Handle destination ---
    std::string dstParent, dstName;
    splitParentPath(destpath, dstParent, dstName);

    uint16_t dstDirBlock;
    if (resolvePath(dstParent, true, dstDirBlock) != 0)
        return 4; // invalid dest parent

    // Load destination directory
    uint8_t dstDirBuf[BLOCK_SIZE];
    if (disk.read(dstDirBlock, dstDirBuf) != 0)
        return 5;
    dir_entry* dstEntries = reinterpret_cast<dir_entry*>(dstDirBuf);

    // If destpath refers to an existing directory, copy inside it with same name
    if (!dstName.empty()) {
        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
            if (dstEntries[i].file_name[0] != '\0' &&
                strcmp(dstEntries[i].file_name, dstName.c_str()) == 0 &&
                dstEntries[i].type == TYPE_DIR) {
                // It's a directory → adjust
                dstParent = destpath;
                dstName = srcName;
                if (resolvePath(dstParent, true, dstDirBlock) != 0)
                    return 6;
                if (disk.read(dstDirBlock, dstDirBuf) != 0)
                    return 7;
                dstEntries = reinterpret_cast<dir_entry*>(dstDirBuf);
                break;
            }
        }
    } else {
        // Path ended with "/" → means directory, keep same name
        dstName = srcName;
    }

    // Ensure file with dstName doesn’t already exist
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (dstEntries[i].file_name[0] != '\0' &&
            strcmp(dstEntries[i].file_name, dstName.c_str()) == 0) {
            return 8; // already exists
        }
    }

    // --- Load FAT ---
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 9;

    // --- Copy file data into string ---
    std::string fileData;
    int16_t block = srcFile->first_blk;
    int bytesLeft = srcFile->size;
    while (block != FAT_EOF && bytesLeft > 0) {
        uint8_t buf[BLOCK_SIZE];
        if (disk.read(block, buf) != 0)
            return 10;
        int bytesToRead = std::min(bytesLeft, BLOCK_SIZE);
        fileData.append(reinterpret_cast<char*>(buf), bytesToRead);
        bytesLeft -= bytesToRead;
        block = fat[block];
    }

    // --- Allocate new blocks for copy ---
    std::vector<uint16_t> freeBlocks;
    int bytesWritten = 0;
    int bytesLeftToWrite = fileData.size();
    while (bytesLeftToWrite > 0) {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) {
            if (fat[i] == FAT_FREE) {
                freeBlock = i;
                break;
            }
        }
        if (freeBlock == -1) return 11; // no free blocks

        freeBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE] = {};
        int bytesToWrite = std::min(bytesLeftToWrite, BLOCK_SIZE);
        memcpy(buf, fileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, buf) != 0)
            return 12;

        bytesWritten += bytesToWrite;
        bytesLeftToWrite -= bytesToWrite;
    }

    // --- Link blocks in FAT ---
    for (int i = 0; i < freeBlocks.size(); i++) {
        fat[freeBlocks[i]] = (i + 1 < freeBlocks.size()) ? freeBlocks[i + 1] : FAT_EOF;
    }
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 13;

    // --- Create new dir_entry in destination ---
    dir_entry newFile{};
    strncpy(newFile.file_name, dstName.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF : freeBlocks[0];
    newFile.size = fileData.size();
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE;

    bool inserted = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (dstEntries[i].file_name[0] == '\0') {
            dstEntries[i] = newFile;
            inserted = true;
            break;
        }
    }
    if (!inserted) return 14; // no space in directory

    if (disk.write(dstDirBlock, dstDirBuf) != 0)
        return 15;

    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int 
FS::mv(std::string sourcepath, std::string destpath)
{
    if (sourcepath == destpath) return 0;

    // --- Split source path ---
    std::string srcParent, srcName;
    splitParentPath(sourcepath, srcParent, srcName);

    uint16_t srcDirBlock;
    if (resolvePath(srcParent, true, srcDirBlock) != 0)
        return 1; // invalid source parent

    // --- Load source dir ---
    uint8_t srcBuf[BLOCK_SIZE];
    if (disk.read(srcDirBlock, srcBuf) != 0)
        return 2;
    dir_entry* srcEntries = reinterpret_cast<dir_entry*>(srcBuf);

    // --- Find source entry ---
    dir_entry* srcFile = nullptr;
    int numEntries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < numEntries; i++) {
        if (srcEntries[i].file_name[0] != '\0' &&
            strcmp(srcEntries[i].file_name, srcName.c_str()) == 0) {
            srcFile = &srcEntries[i];
            break;
        }
    }
    if (!srcFile) return 3; // not found

    // --- Split dest path ---
    std::string dstParent, dstName;
    splitParentPath(destpath, dstParent, dstName);

    uint16_t dstDirBlock;
    if (resolvePath(dstParent, true, dstDirBlock) != 0)
        return 4; // invalid dest parent

    // --- Load dest dir ---
    uint8_t dstBuf[BLOCK_SIZE];
    if (disk.read(dstDirBlock, dstBuf) != 0)
        return 5;
    dir_entry* dstEntries = reinterpret_cast<dir_entry*>(dstBuf);

    // --- If dest is an existing directory, move into it ---
    if (!dstName.empty()) {
        for (int i = 0; i < numEntries; i++) {
            if (dstEntries[i].file_name[0] != '\0' &&
                strcmp(dstEntries[i].file_name, dstName.c_str()) == 0 &&
                dstEntries[i].type == TYPE_DIR) {
                // move into that directory, keep name
                dstParent = destpath;
                dstName = srcName;
                if (resolvePath(dstParent, true, dstDirBlock) != 0)
                    return 6;
                if (disk.read(dstDirBlock, dstBuf) != 0)
                    return 7;
                dstEntries = reinterpret_cast<dir_entry*>(dstBuf);
                break;
            }
        }
    } else {
        // path ended with '/', so use same name
        dstName = srcName;
    }

    // --- Prevent overwrite ---
    for (int i = 0; i < numEntries; i++) {
        if (dstEntries[i].file_name[0] != '\0' &&
            strcmp(dstEntries[i].file_name, dstName.c_str()) == 0) {
            return 8; // already exists
        }
    }

    // --- Insert entry into dest dir ---
    bool inserted = false;
    for (int i = 0; i < numEntries; i++) {
        if (dstEntries[i].file_name[0] == '\0') {
            dir_entry moved = *srcFile;
            strncpy(moved.file_name, dstName.c_str(), sizeof(moved.file_name) - 1);
            moved.file_name[sizeof(moved.file_name) - 1] = '\0';
            dstEntries[i] = moved;
            inserted = true;
            break;
        }
    }
    if (!inserted) return 9; // dest dir full

    if (disk.write(dstDirBlock, dstBuf) != 0)
        return 10;

    // --- Clear source entry ---
    memset(srcFile, 0, sizeof(dir_entry));
    if (disk.write(srcDirBlock, srcBuf) != 0)
        return 11;

    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int 
FS::rm(std::string filepath)
{
    // --- 1. Split into parent + filename ---
    std::string parentPath, name;
    splitParentPath(filepath, parentPath, name);

    // --- 2. Resolve parent directory ---
    uint16_t parentBlock;
    if (resolvePath(parentPath, true, parentBlock) != 0)
        return 1; // invalid parent path

    // --- 3. Read parent directory ---
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(parentBlock, dirBuffer) != 0)
        return 2;

    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    dir_entry* entryToRemove = nullptr;
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);

    for (int i = 0; i < number_of_entries; i++) {
        if (dir_entries[i].file_name[0] != '\0' &&
            strcmp(dir_entries[i].file_name, name.c_str()) == 0) {
            entryToRemove = &dir_entries[i];
            break;
        }
    }
    if (!entryToRemove) return 3; // not found

    // --- 4. Handle directory case ---
    if (entryToRemove->type == TYPE_DIR) {
        uint8_t subDirBuffer[BLOCK_SIZE];
        if (disk.read(entryToRemove->first_blk, subDirBuffer) != 0)
            return 4;

        dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);

        bool empty = true;
        for (int i = 0; i < number_of_entries; i++) {
            if (subDir_entries[i].file_name[0] != '\0' &&
                strcmp(subDir_entries[i].file_name, "..") != 0) {
                empty = false;
                break;
            }
        }
        if (!empty) {
            std::cout << "ERROR: can't remove non-empty directory" << std::endl;
            return 5;
        }
    }

    // --- 5. Free blocks in FAT ---
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 6;

    int16_t currentBlock = static_cast<int16_t>(entryToRemove->first_blk);

    if (currentBlock != 0xFFFF) { // <- guard for empty file
        while (currentBlock != FAT_EOF) {
            int16_t nextBlock = fat[currentBlock];
            fat[currentBlock] = FAT_FREE;
            currentBlock = nextBlock;
        }
    }

    // --- 6. Clear directory entry ---
    memset(entryToRemove, 0, sizeof(dir_entry));

    if (disk.write(parentBlock, dirBuffer) != 0)
        return 7;

    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 8;

    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int 
FS::append(std::string filepath1, std::string filepath2)
{
    // --- 1. Resolve and find source file ---
    std::string srcParent, srcName;
    splitParentPath(filepath1, srcParent, srcName);

    uint16_t srcDirBlock;
    if (resolvePath(srcParent, true, srcDirBlock) != 0)
        return 1;

    uint8_t srcBuf[BLOCK_SIZE];
    if (disk.read(srcDirBlock, srcBuf) != 0)
        return 2;

    dir_entry* srcEntries = reinterpret_cast<dir_entry*>(srcBuf);
    dir_entry* sourceFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (strcmp(srcEntries[i].file_name, srcName.c_str()) == 0) {
            sourceFile = &srcEntries[i];
            break;
        }
    }
    if (!sourceFile || sourceFile->type != TYPE_FILE)
        return 3;

    // --- 2. Resolve and find destination file ---
    std::string dstParent, dstName;
    splitParentPath(filepath2, dstParent, dstName);

    uint16_t dstDirBlock;
    if (resolvePath(dstParent, true, dstDirBlock) != 0)
        return 4;

    uint8_t dstBuf[BLOCK_SIZE];
    if (disk.read(dstDirBlock, dstBuf) != 0)
        return 5;

    dir_entry* dstEntries = reinterpret_cast<dir_entry*>(dstBuf);
    dir_entry* destFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (strcmp(dstEntries[i].file_name, dstName.c_str()) == 0) {
            destFile = &dstEntries[i];
            break;
        }
    }
    if (!destFile || destFile->type != TYPE_FILE)
        return 6;

    // --- 3. Load FAT ---
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 7;

    // --- 4. Read entire source file into memory ---
    std::string sourceFileData;
    int16_t currentBlock = sourceFile->first_blk;
    int bytesLeft = sourceFile->size;

    while (currentBlock != FAT_EOF && bytesLeft > 0) {
        uint8_t buf[BLOCK_SIZE];
        if (disk.read(currentBlock, buf) != 0)
            return 8;

        int bytesToRead = std::min(bytesLeft, BLOCK_SIZE);
        sourceFileData.append(reinterpret_cast<char*>(buf), bytesToRead);

        bytesLeft -= bytesToRead;
        currentBlock = fat[currentBlock];
    }

    if (sourceFileData.empty())
        return 0; // nothing to append

    // --- 5. Append to dest ---
    int sourceFileBytesLeft = sourceFileData.size();
    int bytesWritten = 0;
    std::vector<int16_t> destNewBlocks;

    if (destFile->first_blk != 0xFFFF) {
        // Find last block
        int16_t lastBlock = destFile->first_blk;
        while (fat[lastBlock] != FAT_EOF)
            lastBlock = fat[lastBlock];

        // See if last block has free space
        int usedBytes = destFile->size % BLOCK_SIZE;
        if (usedBytes != 0 && sourceFileBytesLeft > 0) {
            uint8_t blockBuf[BLOCK_SIZE];
            if (disk.read(lastBlock, blockBuf) != 0)
                return 9;

            int freeSpace = BLOCK_SIZE - usedBytes;
            int bytesToCopy = std::min(sourceFileBytesLeft, freeSpace);

            memcpy(blockBuf + usedBytes, sourceFileData.data() + bytesWritten, bytesToCopy);

            if (disk.write(lastBlock, blockBuf) != 0)
                return 9;

            bytesWritten += bytesToCopy;
            sourceFileBytesLeft -= bytesToCopy;
        }
    }

    // Allocate new blocks if needed
    while (sourceFileBytesLeft > 0) {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) {
            if (fat[i] == FAT_FREE) { freeBlock = i; break; }
        }
        if (freeBlock == -1) return 10; // no space

        destNewBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE] = {};
        int bytesToWrite = std::min(sourceFileBytesLeft, BLOCK_SIZE);
        memcpy(buf, sourceFileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, buf) != 0)
            return 11;

        bytesWritten += bytesToWrite;
        sourceFileBytesLeft -= bytesToWrite;
    }

    // Link new blocks
    for (int i = 0; i < (int)destNewBlocks.size(); i++) {
        fat[destNewBlocks[i]] = (i + 1 < (int)destNewBlocks.size())
                              ? destNewBlocks[i + 1]
                              : FAT_EOF;
    }

    // Attach to destination file
    if (destFile->first_blk == 0xFFFF) {
        destFile->first_blk = destNewBlocks[0];
    } else if (!destNewBlocks.empty()) {
        int16_t lastBlock = destFile->first_blk;
        while (fat[lastBlock] != FAT_EOF)
            lastBlock = fat[lastBlock];
        fat[lastBlock] = destNewBlocks[0];
    }

    // Update size
    destFile->size += sourceFileData.size();

    // --- 6. Save back ---
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
        return 12;
    if (disk.write(dstDirBlock, dstBuf) != 0)
        return 13;

    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int 
FS::mkdir(std::string dirpath) {
    // 1. Split path into parent + name
    std::string parentPath, newName;
    splitParentPath(dirpath, parentPath, newName);

    // 2. Resolve parent directory
    uint16_t parentBlock;
    int res = resolvePath(parentPath, true, parentBlock);
    if (res != 0) return res;

    // 3. Check if name already exists in parent
    uint8_t buf[BLOCK_SIZE];
    if (disk.read(parentBlock, buf) != 0) return 1;
    dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (strcmp(entries[i].file_name, newName.c_str()) == 0) {
            return 2; // already exists
        }
    }

    // 4. Allocate a free block for the new directory
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0) return 3;
    int16_t newBlock = -1;
    for (int i = 0; i < disk.get_no_blocks(); i++) {
        if (fat[i] == FAT_FREE) {
            newBlock = i;
            break;
        }
    }
    if (newBlock == -1) return 4; // no space
    fat[newBlock] = FAT_EOF;

    // 5. Initialize new directory block
    uint8_t newBuf[BLOCK_SIZE];
    memset(newBuf, 0, BLOCK_SIZE);
    dir_entry* newEntries = reinterpret_cast<dir_entry*>(newBuf);

    // '..' entry
    strcpy(newEntries[0].file_name, "..");
    newEntries[0].first_blk = parentBlock;
    newEntries[0].type = TYPE_DIR;
    newEntries[0].size = 0;

    disk.write(newBlock, newBuf);

    // 6. Add new entry in parent directory
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) {
        if (entries[i].file_name[0] == '\0') {
            strcpy(entries[i].file_name, newName.c_str());
            entries[i].first_blk = newBlock;
            entries[i].type = TYPE_DIR;
            entries[i].size = 0;
            break;
        }
    }
    disk.write(parentBlock, buf);

    // 7. Save FAT
    disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat));

    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    uint16_t block;
    int res = resolvePath(dirpath, true, block);
    if (res != 0) return res;
    currentDirectory = block;
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int
FS::pwd()
{
    if (currentDirectory == ROOT_BLOCK)
    {
        std::cout << "/" << std::endl;
        return 0;
    }

    std::vector<std::string>filePath;
    uint16_t currentBlock = currentDirectory;

    while (currentBlock != ROOT_BLOCK)
    {
        uint8_t currentBuffer[BLOCK_SIZE];
        if (disk.read(currentBlock, currentBuffer) != 0)
        {
            return 1;
        }
        dir_entry* current_entries = reinterpret_cast<dir_entry*>(currentBuffer);

        uint16_t parentBlock = current_entries[0].first_blk;
        uint8_t parentBuffer[BLOCK_SIZE];
        if (disk.read(parentBlock, parentBuffer) != 0)
        {
            return 2;
        }

        dir_entry* parent_entries = reinterpret_cast<dir_entry*>(parentBuffer);

        std::string currentPath = "";
        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
        {
            if (parent_entries[i].file_name[0] != '\0')
            {
                if (parent_entries[i].first_blk == currentBlock)
                {
                    if (parent_entries[i].type == TYPE_DIR)
                    {
                        currentPath = parent_entries[i].file_name;
                        break;
                    }
                }
            }
        }

        filePath.push_back(currentPath);
        currentBlock = parentBlock;
    }

    std::cout << "/";
    for (int i = (int)filePath.size() - 1; i >= 0; i--)
    {
        std::cout << filePath[i];
        if (i != 0)
        {
            std::cout << "/";
        }
    }
    std::cout << std::endl;

    return 0;
}

// chmod <accessrights> <filepath> changes the access rights for the
// file <filepath> to <accessrights>.
int
FS::chmod(std::string accessrights, std::string filepath)
{
    std::cout << "FS::chmod(" << accessrights << "," << filepath << ")\n";
    return 0;
}