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

// Splits a path into parent path + filename in the form of:
// path = "/folder/file.txt" into parent = "/folder" + name = "file.txt"
void
FS::splitParentPath(const std::string& path, std::string& parent, std::string& name) 
{
    // Find last '/' in the path
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) 
    {
        // No '/' means parent is current directory
        parent = "";
        name = path;
    } 
    else if (pos == 0) 
    {
        // Path like "/file" means parent is root
        parent = "/";
        name = path.substr(1);
    } 
    else
    {
        // Default case
        parent = path.substr(0, pos);
        name = path.substr(pos + 1);
    }
}
//                  Path to resolve | Directory Flag | Disk block requested
int              
FS::resolvePath(const std::string& path, bool mustBeDir, uint16_t& outBlock) 
{
    // Determine absolute or relative path
    uint16_t current = (path.size() > 0 && path[0] == '/') ? ROOT_BLOCK : currentDirectory;

    // Split by '/'
    std::stringstream ss(path);
    std::string token;
    std::vector<std::string> tokens;
    while (std::getline(ss, token, '/')) 
    {
        if (!token.empty())
        {
            tokens.push_back(token);
        } 
    }

    // Empty path just means current directory
    if (tokens.empty()) 
    {
        outBlock = current;
        return 0;
    }

    // Traverse
    for (size_t i = 0; i < tokens.size(); i++) 
    {
        const std::string& part = tokens[i];

        if (part == "..") 
        {
            if (current == ROOT_BLOCK) 
            {
                continue; // stay at root
            }

            uint8_t buf[BLOCK_SIZE];
            if (disk.read(current, buf) != 0)
            {
                return -1;
            }
            dir_entry* entries = reinterpret_cast<dir_entry*>(buf);
            current = entries[0].first_blk; // parent pointer
            continue;
        }

        // Must search current directory for part
        uint8_t buf[BLOCK_SIZE];
        if (disk.read(current, buf) != 0)
        {
            return -2;
        } 
        dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

        bool found = false;
        for (int j = 0; j < BLOCK_SIZE / sizeof(dir_entry); j++) 
        {
            if (strcmp(entries[j].file_name, part.c_str()) == 0) 
            {
                // check execute rights when stepping into a directory
                if (entries[j].type == TYPE_DIR) 
                {
                    if (!(entries[j].access_rights & EXECUTE)) 
                    {
                        std::cout << "ERROR: no execute rights on directory " << entries[j].file_name << std::endl;
                        return -3;
                    }
                }

                // If it's the last token, check if it's a directory
                if (i == tokens.size() - 1) 
                {
                    if (mustBeDir && entries[j].type != TYPE_DIR)
                    {
                        return -4;
                    } 

                    current = entries[j].first_blk;
                    found = true;
                    break;
                } 
                else 
                {
                    // must be directory if not last
                    if (entries[j].type != TYPE_DIR)
                    {
                        return -5;
                    }             
                    
                    current = entries[j].first_blk;
                    found = true;
                    break;
                }
            }
        }
        if (!found) 
        {
            return -6; // not found
        }
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
    for (int i = 2; i < number_of_blocks; i++) 
    {
        fat[i] = FAT_FREE;
    }

    // Initialize root directory block
    uint8_t rootBuf[BLOCK_SIZE];
    memset(rootBuf, 0, BLOCK_SIZE);
    dir_entry* rootEntries = reinterpret_cast<dir_entry*>(rootBuf);

    // root's ".." points to itself for consistency with helper functions
    strcpy(rootEntries[0].file_name, "..");
    rootEntries[0].first_blk = ROOT_BLOCK;  
    rootEntries[0].type = TYPE_DIR;
    rootEntries[0].size = 0;
    rootEntries[0].access_rights = READ | WRITE | EXECUTE;  // Default for root: RWX

    // Write root block
    if (disk.write(ROOT_BLOCK, rootBuf) != 0)
    {
        return 1;
    }           
    // Write formatted FAT to disk
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 2;
    }

    // Clear all other blocks
    uint8_t emptyBuf[BLOCK_SIZE];
    memset(emptyBuf, 0, BLOCK_SIZE);
    for (int i = 2; i < number_of_blocks; i++) 
    {
        if (disk.write(i, emptyBuf) != 0)
        {
            return 3;
        }
    }

    return 0;
}

// create <filepath> creates a new file on the disk, the data content is
// written on the following rows (ended with an empty row)
int 
FS::create(std::string filepath)
{
    // Split into parent path + file name
    std::string parentPath, name;
    splitParentPath(filepath, parentPath, name);

    // Filename length check
    if (name.size() >= 56) 
    {
        return 1;
    }

    // Resolve parent directory
    uint16_t parentBlock;
    int retVal = resolvePath(parentPath, true, parentBlock);
    if (retVal != 0) 
    {
        return retVal;
    }
    
    // Read parent directory
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(parentBlock, dirBuffer) != 0)
    {
        return 2;
    }
    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    
    // Check write permission on parent directory
    if (!(directory_entries[0].access_rights & WRITE)) 
    {
        std::cout << "No write rights on parent directory" << std::endl;
        return 3;
    }

    // Check for duplicate file
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (directory_entries[i].file_name[0] != '\0' && strcmp(directory_entries[i].file_name, name.c_str()) == 0) 
        {
            return 4;
        }
    }

    // Read file data from stdin
    std::string completedText, line;
    while (true) 
    {
        if (!std::getline(std::cin, line))
        {
            break;
        } 
        if (line.empty())
        {
            break;
        } 
        completedText.append(line + "\n");
    }

    // Load FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 5;
    }

    // Allocate blocks for new file
    std::vector<uint16_t> freeBlocks;
    int textLeft = completedText.size();
    int textWritten = 0;

    while (textLeft > 0) 
    {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) 
        {
            if (fat[i] == FAT_FREE) 
            { 
                freeBlock = i;
                break; 
            }
        }
        // No free blocks in FAT
        if (freeBlock == -1)
        {
            return 6;
        } 

        freeBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE]{};
        int toWrite = std::min(textLeft, BLOCK_SIZE);
        memcpy(buf, completedText.data() + textWritten, toWrite);

        if (disk.write(freeBlock, buf) != 0)
        {
            return 7;
        } 

        textWritten += toWrite;
        textLeft -= toWrite;
    }

    // Update FAT
    for (int i = 0; i < (int)freeBlocks.size(); i++) 
    {
        fat[freeBlocks[i]] = (i + 1 < (int)freeBlocks.size()) ? freeBlocks[i + 1] : FAT_EOF;
    }

    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 8;
    }
        
    // Create dir_entry
    dir_entry newFile{};
    strncpy(newFile.file_name, name.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.size = completedText.size();
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF : freeBlocks[0];
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE;

    // Insert into parent directory
    bool inserted = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (directory_entries[i].file_name[0] == '\0') 
        {
            directory_entries[i] = newFile;
            inserted = true;
            break;
        }
    }
    // Directory full
    if (!inserted) 
    {
        return 9; 
    }

    if (disk.write(parentBlock, dirBuffer) != 0)
    {
        return 10;
    }
        
    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
// in the format of "Folder/SubFolder/File" or "/Folder/SubFolder/file"
int
FS::cat(std::string filepath) 
{
    // Find the parent directory and entry
    std::string parentPath, filename;
    splitParentPath(filepath, parentPath, filename);

    uint16_t parentBlock;
    int retVal = resolvePath(parentPath, true, parentBlock);
    if (retVal != 0) 
    {
        return retVal;
    }

    uint8_t buf[BLOCK_SIZE];
    if (disk.read(parentBlock, buf) != 0)
    {
        return 1;
    }

    dir_entry* entries = reinterpret_cast<dir_entry*>(buf);
    dir_entry* targetFile = nullptr;

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (strcmp(entries[i].file_name, filename.c_str()) == 0) 
        {
            targetFile = &entries[i];
            break;
        }
    }

    if (!targetFile) 
    {
        return 2;
    }
    if (targetFile->type == TYPE_DIR)
    {
        return 3;
    } 
    if (!(targetFile->access_rights & READ))
    {
        //std::cout << "ERROR: no read permission\n";
        return 4;
    }

    // Load FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 5;
    }

    // Traverse file blocks and print
    int16_t fileBlock = static_cast<int16_t>(targetFile->first_blk);
    int bytesToRead = targetFile->size;

    while (fileBlock != FAT_EOF && bytesToRead > 0) 
    {
        uint8_t blockBuffer[BLOCK_SIZE];
        if (disk.read(fileBlock, blockBuffer) != 0)
        {
            return 6;
        }

        int bytesToPrint = std::min(bytesToRead, BLOCK_SIZE);
        std::cout.write(reinterpret_cast<char*>(blockBuffer), bytesToPrint);

        bytesToRead -= bytesToPrint;
        fileBlock = fat[fileBlock];
    }

    std::cout << std::endl;
    return 0;
}

// Helper function for printing access rights
std::string FS::rightsTripletString(uint8_t rights) 
{
    std::string string = "";
    string += (rights & READ)    ? "r" : "-";
    string += (rights & WRITE)   ? "w" : "-";
    string += (rights & EXECUTE) ? "x" : "-";
    return string;
}

// ls lists the content in the currect directory (files and sub-directories)
int
FS::ls()
{
    // Load in current directory to print
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 1;
    }
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);

    // Header format
    std::cout << std::endl;
    std::cout 
    << std::left << std::setw(20)
    << "name"
    << std::setw(15)
    << "type"
    << std::setw(15)
    << "accessrights"
    << std::setw(15)
    << "size" 
    << std::endl;
    
    std::string type = "";
    
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

        // Get file type
        type = (dir_entries[i].type == TYPE_FILE) ? "file" : "dir";
       
        std::cout 
        << std::left << std::setw(20) 
        << dir_entries[i].file_name 
        << std::setw(15) 
        << type 
        << std::setw(15) 
        << rightsTripletString(dir_entries[i].access_rights)
        << std::setw(15) 
        << ((type == "dir") ? "-" : std::to_string(dir_entries[i].size)) // "-" if tpye is directory, otherwise file size
        << std::endl; 
    }
    std::cout << std::endl;
    
    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int 
FS::cp(std::string sourcepath, std::string destpath)
{
    // Split source path
    std::string sourceParent, sourceName;
    splitParentPath(sourcepath, sourceParent, sourceName);

    // Resolve source parent directory
    uint16_t sourceDirBlock;
    int retVal = resolvePath(sourceParent, true, sourceDirBlock);
    if (retVal != 0)
    {   
        return retVal; // source parent invalid
    }
        
    // Find source file
    uint8_t sourceDirBuf[BLOCK_SIZE];
    if (disk.read(sourceDirBlock, sourceDirBuf) != 0)
    {
        return 2;
    }
        
    dir_entry* sourceEntries = reinterpret_cast<dir_entry*>(sourceDirBuf);
    dir_entry* sourceFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (sourceEntries[i].file_name[0] != '\0' && strcmp(sourceEntries[i].file_name, sourceName.c_str()) == 0) 
        {
            sourceFile = &sourceEntries[i];
            break;
        }
    }

    if (!sourceFile || sourceFile->type != TYPE_FILE)
    {
        return 3; // source not found or is a dir
    }
     // Access rights: must be readable
    if (!(sourceFile->access_rights & READ)) 
    {
        std::cout << "ERROR: no READ permission on " << sourceFile->file_name << "\n";
        return 4;
    }

    // Handle destination
    std::string destParent, destName;
    splitParentPath(destpath, destParent, destName);

    uint16_t destDirBlock;
    retVal = resolvePath(destParent, true, destDirBlock);
    if (retVal != 0)
    {
        return retVal; // invalid dest parent
    }
        
    // Load destination directory
    uint8_t destDirBuf[BLOCK_SIZE];
    if (disk.read(destDirBlock, destDirBuf) != 0)
    {
        return 5;
    }
    
    dir_entry* destEntries = reinterpret_cast<dir_entry*>(destDirBuf);

    // Check WRITE on destination parent
    if (destDirBlock != ROOT_BLOCK) 
    {
        if (!(destEntries[0].access_rights & WRITE)) 
        {
            std::cout << "No WRITE rights on destination parent directory\n";
            return 6;
        }
    }

    // If destpath refers to an existing directory, copy inside it with same name
    if (!destName.empty()) 
    {
        for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
        {
            if (destEntries[i].file_name[0] != '\0' && strcmp(destEntries[i].file_name, destName.c_str()) == 0 && destEntries[i].type == TYPE_DIR) 
            {
                // Adjust if it's a directory
                destDirBlock = destEntries[i].first_blk; 
                destName = sourceName;

                if (disk.read(destDirBlock, destDirBuf) != 0)
                {
                    return 7;
                }
                    
                destEntries = reinterpret_cast<dir_entry*>(destDirBuf);
                break;
            }
        }
    } 
    else
    {
        // Path ended with "/" → means directory, keep same name
        destName = sourceName;
    }

    // Ensure file with destName doesn’t already exist
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (destEntries[i].file_name[0] != '\0' && strcmp(destEntries[i].file_name, destName.c_str()) == 0) 
        {
            return 8; // already exists
        }
    }

    // Load FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 9;
    }
        
    // Copy file data into string
    std::string fileData;
    int16_t block = sourceFile->first_blk;
    int bytesLeft = sourceFile->size;

    while (block != FAT_EOF && bytesLeft > 0) 
    {
        uint8_t buf[BLOCK_SIZE];
        if (disk.read(block, buf) != 0)
        {
            return 10;
        }
            
        int bytesToRead = std::min(bytesLeft, BLOCK_SIZE);
        fileData.append(reinterpret_cast<char*>(buf), bytesToRead);
        bytesLeft -= bytesToRead;
        block = fat[block];
    }

    // Allocate new blocks for copy
    std::vector<uint16_t> freeBlocks;
    int bytesWritten = 0;
    int bytesLeftToWrite = fileData.size();
    while (bytesLeftToWrite > 0) 
    {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) 
        {
            if (fat[i] == FAT_FREE) 
            {
                freeBlock = i;
                break;
            }
        }

        if (freeBlock == -1)
        {
            return 11; // no free blocks
        } 

        freeBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE]{};
        int bytesToWrite = std::min(bytesLeftToWrite, BLOCK_SIZE);
        memcpy(buf, fileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, buf) != 0)
        {
            return 12;
        }

        bytesWritten += bytesToWrite;
        bytesLeftToWrite -= bytesToWrite;
    }

    // Link blocks in FAT
    for (int i = 0; i < freeBlocks.size(); i++) 
    {
        fat[freeBlocks[i]] = (i + 1 < freeBlocks.size()) ? freeBlocks[i + 1] : FAT_EOF;
    }

    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 13;
    }   

    // Create new dir_entry in destination
    dir_entry newFile{};
    strncpy(newFile.file_name, destName.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF : freeBlocks[0];
    newFile.size = fileData.size();
    newFile.type = TYPE_FILE;
    newFile.access_rights = sourceFile->access_rights;

    bool inserted = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (destEntries[i].file_name[0] == '\0') 
        {
            destEntries[i] = newFile;
            inserted = true;
            break;
        }
    }

    if (!inserted)
    {
        return 14; // no space in directory
    } 

    if (disk.write(destDirBlock, destDirBuf) != 0)
    {
        return 15;
    }
        
    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int 
FS::mv(std::string sourcepath, std::string destpath)
{
    if (sourcepath == destpath)
    {
        return 0;
    } 

    // Split source path
    std::string sourceParent, sourceName;
    splitParentPath(sourcepath, sourceParent, sourceName);

    uint16_t sourceDirBlock;
    int retVal = resolvePath(sourceParent, true, sourceDirBlock);
    if (retVal != 0)
    {
        return retVal; // invalid source parent
    }

    // Load source dir
    uint8_t sourceBuf[BLOCK_SIZE];
    if (disk.read(sourceDirBlock, sourceBuf) != 0)
    {
        return 1;
    }
        
    dir_entry* sourceEntries = reinterpret_cast<dir_entry*>(sourceBuf);

    // Find source entry
    dir_entry* sourceFile = nullptr;
    int numEntries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < numEntries; i++) 
    {
        if (sourceEntries[i].file_name[0] != '\0' && strcmp(sourceEntries[i].file_name, sourceName.c_str()) == 0) 
        {
            sourceFile = &sourceEntries[i];
            break;
        }
    }

    if (!sourceFile) 
    {
        return 2; // not found
    }

     // Check WRITE on source parent
    if (sourceDirBlock != ROOT_BLOCK) 
    {
        if (!(sourceEntries[0].access_rights & WRITE)) 
        {
            std::cout << "No WRITE rights on source parent directory\n";
            return 3;
        }
    }

    // Split dest path
    std::string destParent, destName;
    splitParentPath(destpath, destParent, destName);

    uint16_t destDirBlock;
    retVal = resolvePath(destParent, true, destDirBlock);
    if (retVal != 0)
    {
        return retVal; // invalid dest parent
    }

    // Load dest dir
    uint8_t destBuf[BLOCK_SIZE];
    if (disk.read(destDirBlock, destBuf) != 0)
    {
        return 4;
    }
        
    dir_entry* destEntries = reinterpret_cast<dir_entry*>(destBuf);

    // Check write on destination parent
    if (destDirBlock != ROOT_BLOCK) 
    {
        if (!(destEntries[0].access_rights & WRITE)) 
        {
            std::cout << "No WRITE rights on destination parent directory\n";
            return 5;
        }
    }

    // If dest is an existing directory, move into it
    if (!destName.empty()) 
    {
        for (int i = 0; i < numEntries; i++) 
        {
            if (destEntries[i].file_name[0] != '\0' && strcmp(destEntries[i].file_name, destName.c_str()) == 0 && destEntries[i].type == TYPE_DIR) 
            {
                // move into that directory, keep name
                destDirBlock = destEntries[i].first_blk;   // direct jump
                destName = sourceName;

                if (disk.read(destDirBlock, destBuf) != 0)
                {
                    return 6;
                }
                    
                destEntries = reinterpret_cast<dir_entry*>(destBuf);
                break;
            }
        }
    } 
    else 
    {
        // path ended with '/', so use same name
        destName = sourceName;
    }

    // Prevent overwrite
    for (int i = 0; i < numEntries; i++) 
    {
        if (destEntries[i].file_name[0] != '\0' && strcmp(destEntries[i].file_name, destName.c_str()) == 0) 
        {
            return 7; // already exists
        }
    }

    // Insert entry into dest dir
    bool inserted = false;
    for (int i = 0; i < numEntries; i++) 
    {
        if (destEntries[i].file_name[0] == '\0') 
        {
            dir_entry moved = *sourceFile;
            strncpy(moved.file_name, destName.c_str(), sizeof(moved.file_name) - 1);
            moved.file_name[sizeof(moved.file_name) - 1] = '\0';
            destEntries[i] = moved;
            inserted = true;
            break;
        }
    }
    if (!inserted) 
    {
        return 8; // destination directory full
    }

    if (disk.write(destDirBlock, destBuf) != 0)
    {
        return 9;
    }
        
    // Clear source entry
    memset(sourceFile, 0, sizeof(dir_entry));
    if (disk.write(sourceDirBlock, sourceBuf) != 0)
    {
        return 10;
    }
        
    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int 
FS::rm(std::string filepath)
{
    // Split into parent + filename
    std::string parentPath, name;
    splitParentPath(filepath, parentPath, name);

    // Resolve parent directory
    uint16_t parentBlock;
    int retVal = resolvePath(parentPath, true, parentBlock);
    if (retVal != 0)
    {
        return retVal; // invalid parent path
    }
        
    // Read parent directory
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(parentBlock, dirBuffer) != 0)
    {
        return 1;
    }
        
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);

    // Check write permission on parent directory
    if (parentBlock != ROOT_BLOCK) // root always allowed
    {  
        if (!(dir_entries[0].access_rights & WRITE)) 
        {
            std::cout << "No write rights on parent directory" << std::endl;
            return 2;
        }
    }

    // Find entry to remove
    dir_entry* entryToRemove = nullptr;
    for (int i = 0; i < number_of_entries; i++) 
    {
        if (dir_entries[i].file_name[0] != '\0' && strcmp(dir_entries[i].file_name, name.c_str()) == 0) 
        {
            entryToRemove = &dir_entries[i];
            break;
        }
    }
   
    if (!entryToRemove)
    {
        return 3; // not found
    } 

    // Handle directory case
    if (entryToRemove->type == TYPE_DIR) 
    {
        uint8_t subDirBuffer[BLOCK_SIZE];
        if (disk.read(entryToRemove->first_blk, subDirBuffer) != 0)
        {
            return 4;
        }
            
        dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);

        bool empty = true;
        for (int i = 0; i < number_of_entries; i++) 
        {
            if (subDir_entries[i].file_name[0] != '\0' && strcmp(subDir_entries[i].file_name, "..") != 0) 
            {
                empty = false;
                break;
            }
        }

        if (!empty) 
        {
            std::cout << "ERROR: can't remove non-empty directory" << std::endl;
            return 5;
        }
    }

    // Free blocks in FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 6;
    }
        
    int16_t currentBlock = static_cast<int16_t>(entryToRemove->first_blk);
    if (currentBlock != 0xFFFF) // Guard against empty file
    { 
        while (currentBlock != FAT_EOF) 
        {
            int16_t nextBlock = fat[currentBlock];
            fat[currentBlock] = FAT_FREE;
            currentBlock = nextBlock;
        }
    }

    // Clear directory entry
    memset(entryToRemove, 0, sizeof(dir_entry));

    if (disk.write(parentBlock, dirBuffer) != 0)
    {
        return 7;
    }
        
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 8;
    }

    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int 
FS::append(std::string filepath1, std::string filepath2)
{
    // Resolve and find source file
    std::string sourceParent, sourceName;
    splitParentPath(filepath1, sourceParent, sourceName);

    uint16_t sourceDirBlock;
    int retVal = resolvePath(sourceParent, true, sourceDirBlock);
    if (retVal != 0)
    {
        return retVal;
    }
        
    uint8_t sourceBuf[BLOCK_SIZE];
    if (disk.read(sourceDirBlock, sourceBuf) != 0)
    {
        return 1;
    }

    dir_entry* sourceEntries = reinterpret_cast<dir_entry*>(sourceBuf);
    dir_entry* sourceFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (strcmp(sourceEntries[i].file_name, sourceName.c_str()) == 0) 
        {
            sourceFile = &sourceEntries[i];
            break;
        }
    }

    if (!sourceFile || sourceFile->type != TYPE_FILE)
    {
        return 2;
    }
    if (!(sourceFile->access_rights & READ)) 
    {
        std::cout << "No READ rights on source file" << std::endl;
        return 3;
    }

    // Resolve and find destination file
    std::string destParent, destName;
    splitParentPath(filepath2, destParent, destName);

    uint16_t destDirBlock;
    retVal = resolvePath(destParent, true, destDirBlock); 
    if (retVal != 0)
    {
        return retVal;
    }
        
    uint8_t destBuf[BLOCK_SIZE];
    if (disk.read(destDirBlock, destBuf) != 0)
    {
        return 4;
    }
        
    dir_entry* destEntries = reinterpret_cast<dir_entry*>(destBuf);
    dir_entry* destFile = nullptr;

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (strcmp(destEntries[i].file_name, destName.c_str()) == 0) 
        {
            destFile = &destEntries[i];
            break;
        }
    }

    if (!destFile || destFile->type != TYPE_FILE)
    {
        return 5;
    }
    if (!(destFile->access_rights & WRITE))
    {
        std::cout << "No write access on destination file" << std::endl;
        return 6;
    }    

    // Load FAT
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 7;
    }
        
    // Read entire source file into memory
    std::string sourceFileData;
    int16_t currentBlock = sourceFile->first_blk;
    int bytesLeft = sourceFile->size;

    while (currentBlock != FAT_EOF && bytesLeft > 0) 
    {
        uint8_t buf[BLOCK_SIZE]{};
        if (disk.read(currentBlock, buf) != 0)
        {
            return 8;
        }
            
        int bytesToRead = std::min(bytesLeft, BLOCK_SIZE);
        sourceFileData.append(reinterpret_cast<char*>(buf), bytesToRead);

        bytesLeft -= bytesToRead;
        currentBlock = fat[currentBlock];
    }

    if (sourceFileData.empty())
    {
        return 0; // nothing to append, no error, just return safely
    }

    // Append to dest
    int sourceFileBytesLeft = sourceFileData.size();
    int bytesWritten = 0;
    std::vector<int16_t> destNewBlocks;

    if (destFile->first_blk != 0xFFFF) 
    {
        // Find last block
        int16_t lastBlock = destFile->first_blk;
        while (fat[lastBlock] != FAT_EOF)
        {
            lastBlock = fat[lastBlock];
        }
            
        // See if last block has free space
        int usedBytes = destFile->size % BLOCK_SIZE;
        if (usedBytes != 0 && sourceFileBytesLeft > 0) 
        {
            uint8_t blockBuf[BLOCK_SIZE];
            if (disk.read(lastBlock, blockBuf) != 0)
            {
                return 9;
            }
                
            int freeSpace = BLOCK_SIZE - usedBytes;
            int bytesToCopy = std::min(sourceFileBytesLeft, freeSpace);

            memcpy(blockBuf + usedBytes, sourceFileData.data() + bytesWritten, bytesToCopy);

            if (disk.write(lastBlock, blockBuf) != 0)
            {
                return 10;
            }

            bytesWritten += bytesToCopy;
            sourceFileBytesLeft -= bytesToCopy;
        }
    }

    // Allocate new blocks if needed
    while (sourceFileBytesLeft > 0) 
    {
        int freeBlock = -1;
        for (int i = 0; i < disk.get_no_blocks(); i++) 
        {
            if (fat[i] == FAT_FREE) 
            { 
                freeBlock = i; 
                break; 
            }
        }

        // No free blocks in FAT
        if (freeBlock == -1)
        {
            return 11; 
        } 

        destNewBlocks.push_back(freeBlock);

        uint8_t buf[BLOCK_SIZE]{};
        int bytesToWrite = std::min(sourceFileBytesLeft, BLOCK_SIZE);
        memcpy(buf, sourceFileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, buf) != 0)
        {
            return 12;
        }
            
        bytesWritten += bytesToWrite;
        sourceFileBytesLeft -= bytesToWrite;
    }

    // Link new blocks
    for (int i = 0; i < (int)destNewBlocks.size(); i++) 
    {
        fat[destNewBlocks[i]] = (i + 1 < (int)destNewBlocks.size()) ? destNewBlocks[i + 1] : FAT_EOF;
    }

    // Attach to destination file
    if (destFile->first_blk == 0xFFFF) 
    {
        destFile->first_blk = destNewBlocks[0];
    } 
    else if (!destNewBlocks.empty()) 
    {
        int16_t lastBlock = destFile->first_blk;
        while (fat[lastBlock] != FAT_EOF)
        {
            lastBlock = fat[lastBlock];
        }
        fat[lastBlock] = destNewBlocks[0];
    }

    // Update size
    destFile->size += sourceFileData.size();

    // Save back
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 13;
    }
        
    if (disk.write(destDirBlock, destBuf) != 0)
    {
        return 14;
    }
        
    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int 
FS::mkdir(std::string dirpath) 
{
    // Split path into parent + name
    std::string parentPath, newName;
    splitParentPath(dirpath, parentPath, newName);

    // Resolve parent directory
    uint16_t parentBlock;
    int retVal = resolvePath(parentPath, true, parentBlock);
    if (retVal != 0)
    {
        return retVal;
    } 

    // Read parent directory
    uint8_t buf[BLOCK_SIZE];
    if (disk.read(parentBlock, buf) != 0)
    {   
        return 1;
    } 

    dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

    // Check write permission on parent directory
    if (parentBlock != ROOT_BLOCK)   // root always allowed
    {
        if (!(entries[0].access_rights & WRITE)) 
        {
            std::cout << "No WRITE rights on parent directory" << std::endl;
            return 2;
        }
    }

    // Check if name already exists in parent
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (entries[i].file_name[0] != '\0' && strcmp(entries[i].file_name, newName.c_str()) == 0) 
        {
            return 3; // already exists
        }
    }

    // Allocate a free block for the new directory
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 4;
    } 
    
    int16_t newBlock = -1;
    for (int i = 0; i < disk.get_no_blocks(); i++) 
    {
        if (fat[i] == FAT_FREE) 
        {
            newBlock = i;
            break;
        }
    }

    // No free slots in FAT
    if (newBlock == -1)
    {
        return 5;
    } 
    fat[newBlock] = FAT_EOF;

    // Initialize new directory block
    uint8_t newBuf[BLOCK_SIZE];
    memset(newBuf, 0, BLOCK_SIZE);
    dir_entry* newEntries = reinterpret_cast<dir_entry*>(newBuf);

    // Parent '..' entry
    strcpy(newEntries[0].file_name, "..");
    newEntries[0].first_blk = parentBlock;
    newEntries[0].type = TYPE_DIR;
    newEntries[0].size = 0;
    newEntries[0].access_rights = READ | WRITE | EXECUTE;

    disk.write(newBlock, newBuf);

    // Add new entry in parent directory
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++) 
    {
        if (entries[i].file_name[0] == '\0') 
        {
            strcpy(entries[i].file_name, newName.c_str());
            entries[i].first_blk = newBlock;
            entries[i].type = TYPE_DIR;
            entries[i].size = 0;
            entries[i].access_rights = READ | WRITE | EXECUTE;
            break;
        }
    }
    
    // Update parent directory
    if (disk.write(parentBlock, buf) != 0)
    {
        return 6;
    }

    // Update FAT
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 7;
    }

    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    uint16_t targetBlock;
    
    // Update current directory using the output of targetBlock
    int retVal = resolvePath(dirpath, true, targetBlock);
    if (retVal != 0) 
    {
        return retVal;
    }

    currentDirectory = targetBlock;
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int
FS::pwd()
{
    // If already at root, print and return
    if (currentDirectory == ROOT_BLOCK)
    {
        std::cout << "/" << std::endl;
        return 0;
    }

    std::vector<std::string> filePath;
    uint16_t currentBlock = currentDirectory;

    // Traverse up the file hierarchy to find root and save the path taken
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

        // Add current the current path taken to the total path and move up to parent
        filePath.push_back(currentPath);
        currentBlock = parentBlock;
    }

    // Print working directory path
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
    // Parse access rights
    int rights = std::stoi(accessrights);
    if (rights < 0 || rights > 7) 
    {
        return 1;
    }

    // Split path into parent + name
    std::string parentPath, name;
    splitParentPath(filepath, parentPath, name);

    // Resolve parent directory
    uint16_t parentBlock;
    int retVal = resolvePath(parentPath, true, parentBlock);
    if (retVal != 0)
    {
        return retVal;
    }

    // Load parent directory
    uint8_t buf[BLOCK_SIZE];
    if (disk.read(parentBlock, buf) != 0)
    {
        return 2;
    } 
    dir_entry* entries = reinterpret_cast<dir_entry*>(buf);

     // Check write rights on the parent directory
    if (parentBlock != ROOT_BLOCK) 
    {
        if (!(entries[0].access_rights & WRITE)) 
        {
            std::cout << "ERROR: no write rights on parent directory\n";
            return 3;
        }
    }

    // Find target entry
    dir_entry* target = nullptr;
    int numEntries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < numEntries; i++) 
    {
        if (strcmp(entries[i].file_name, name.c_str()) == 0) 
        {
            target = &entries[i];
            break;
        }
    }
    if (!target)
    {
        return 4; // not found
    } 

    // Update rights
    target->access_rights = rights;

    // Save back
    if (disk.write(parentBlock, buf) != 0)
    {
        return 5;
    } 

    return 0;
}