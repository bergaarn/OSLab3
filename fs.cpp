#include <iostream>
#include <cstring>
#include <vector>
#include <iomanip>
#include "fs.h"

FS::FS()
{

}

FS::~FS()
{

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
    for(int i = 2; i < number_of_blocks; i++)
    {
        fat[i] = FAT_FREE;
    }

    // Set block buffer to all 0's
    uint8_t blockBuffer[BLOCK_SIZE];
    for (int i = 0; i < BLOCK_SIZE; i++)
    {
        blockBuffer[i] = 0;
    }

    // Clear root block
    if (disk.write(ROOT_BLOCK, blockBuffer) != 0)
    {
        return 1;
    }
    // Write formatted FAT to disk
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 2;
    }

    // Clear all other blocks
    for (int i = 2; i < number_of_blocks; i++)
    {
        if (disk.write(i, blockBuffer) != 0)
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
    // Check max length for file name
    if (filepath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 1;
    }

    // Check for duplicate file
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 8;
    }
    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(dirBuffer);

    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
    {
        // If filename isnt empty
        if (directory_entries[i].file_name[0] != '\0')  
        {
            // Return if filename already exists
            if (strcmp(directory_entries[i].file_name, filepath.c_str()) == 0)
            {
                return 3;
            }
        }
    }

    bool fileDataRead = false;
    std::string line;
    std::string completedText;
    while (!fileDataRead)
    {
        std::getline(std::cin, line);
        if (line.empty())
        {
            fileDataRead = true;
        }
        else 
        {
            completedText.append(line + "\n");
        }
    }

    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 4;
    }

    std::vector<uint16_t> freeBlocks;
    int textLeft = completedText.size();
    int textWritten = 0;

    while (textLeft > 0)
    {
        int freeBlock = -1;
        int number_of_blocks = disk.get_no_blocks();
        for (int i = 0; i < number_of_blocks; i++)
        {
            // Free block found
            if (fat[i] == FAT_FREE)
            {
                freeBlock = i;
                break;  
            }
        }

        // Return if no blocks are free
        if (freeBlock == -1)
        {
            return 5;
        }

        // Add free block to file's allocated blocks
        freeBlocks.push_back(freeBlock);

        uint8_t textBuffer[BLOCK_SIZE]{};
        // Amount of data to write (full block 4096 or less if last block for file)
        int textToSend = std::min(textLeft, int(BLOCK_SIZE));
        memcpy(textBuffer, completedText.data() + textWritten, textToSend);

        // Return if failed to write to the free block
        if (disk.write(freeBlock, textBuffer) != 0)
        {
            return 6;
        }

        // Update amount of text left to write to blocks
        textWritten += textToSend;
        textLeft -= textToSend;
    }
    
    for (int i = 0; i < freeBlocks.size(); i++)
    {
        if (i + 1 < freeBlocks.size())  // If NOT last block
        {
            fat[freeBlocks[i]] = freeBlocks[i + 1]; // Link first FAT block to the next in chain
        }
        else 
        {
            fat[freeBlocks[i]] = FAT_EOF;   // Set FAT block to last block in chain
        }
    }

    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0) // Update FAT in disk
    {
        return 7;
    } 
    dir_entry newFile{};    // Initialize struct to 0
    strncpy(newFile.file_name, filepath.c_str(), sizeof(newFile.file_name) - 1);    // Copy filepath to struct
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.size = completedText.size();    // Set size of the contents of the file to the new file
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF: freeBlocks[0];
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE; // Read | Write Permissions

    // Find a free `dir_entry` slot (all zeros)
    bool entryAvailable = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i)
    {
        if (directory_entries[i].file_name[0] == '\0')
        {
            directory_entries[i] = newFile;
            entryAvailable = true;
            break;
        }
    }

    // root directory full
    if (!entryAvailable)
    {
        return 9;
    }

    // Write back root block
    if (disk.write(currentDirectory, dirBuffer) != 0)
    {
        return 10;
    }

    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int
FS::cat(std::string filepath)
{
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 1;
    }

    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    dir_entry* targetFile = nullptr;

    for (int i = 0; i < BLOCK_SIZE /sizeof(dir_entry); i++)
    {
        if (directory_entries[i].file_name[0] != '\0')
        {
            if (strcmp(directory_entries[i].file_name, filepath.c_str()) == 0)
            {
                targetFile = &directory_entries[i];
                break;
            }
        }
    }

    if (!targetFile)
    {
        return 2;
    }

    if (targetFile->type == TYPE_DIR)
    {
        //std::cout << "ERROR: file is a directory" << std::endl;
        return 3;
    }

    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 4;
    }

    int16_t fileBlock = static_cast<int16_t>(targetFile->first_blk);
    int bytesToRead = targetFile->size;

    while (fileBlock != FAT_EOF && bytesToRead > 0)
    {
        uint8_t blockBuffer[BLOCK_SIZE];
        if (disk.read(fileBlock, blockBuffer) != 0)
        {
            return 5;
        }      

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
    std::cout << std::left << std::setw(56) << "name" << std::right << "type\t size" << std::endl;
    std::string type = "";
    std::string size = "-";
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
    {
        if (dir_entries[i].file_name[0] == '\0')
        {
            continue;
        }

        if (currentDirectory != ROOT_BLOCK && i == 0)
        {
            continue;
        }

        type = (dir_entries[i].type == TYPE_FILE) ? "file" : "dir";
        if (type == "dir")
        {
            std::cout << std::left << std::setw(56)
            << dir_entries[i].file_name << std::right
            << type << "\t "
            << size << std::endl;
        } 
        else
        {
            std::cout << std::left << std::setw(56)
            << dir_entries[i].file_name << std::right
            << type << "\t "
            << dir_entries[i].size << std::endl;
        }
        
    }

    std::cout << std::endl;

    return 0;
}

// cp <sourcepath> <destpath> makes an exact copy of the file
// <sourcepath> to a new file <destpath>
int
FS::cp(std::string sourcepath, std::string destpath)
{

    // Check max length for file name
    if (destpath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 1;
    }

    if (sourcepath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 2;
    }

    // Read in the root directory
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 3;
    }

    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    dir_entry* sourceFile = nullptr;
    dir_entry* destFile = nullptr;

    bool destpathAlreadyExists = false;
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry); 
    for (int i = 0; i < number_of_entries; i++)
    {
        if (dir_entries[i].file_name[0] != '\0')
        {
            // Find the <sourcepath> in directory
            if(strcmp(dir_entries[i].file_name, sourcepath.c_str()) == 0)
            {
                sourceFile = &dir_entries[i];
            }

            // Check if <destpath> already exists in directory
            if (strcmp(dir_entries[i].file_name, destpath.c_str()) == 0)
            {
                destFile = &dir_entries[i];
            }
        }
    }
    if (!sourceFile)
    {
        return 4;
    }
 
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) !=0)
    {
        return 5;
    }

    // Extract file data from sourcefile's blocks
    std::string fileData;
    int16_t block = static_cast<int16_t>(sourceFile->first_blk);
    int bytesToRead = sourceFile->size;

    while (block != FAT_EOF && bytesToRead > 0)
    {
        uint8_t blockBuffer[BLOCK_SIZE];
        if (disk.read(block, blockBuffer) != 0)
        {
            return 6;
        }

        int bytesToWrite = std::min(bytesToRead, BLOCK_SIZE);
        fileData.append(reinterpret_cast<char*>(blockBuffer), bytesToWrite);

        bytesToRead -= bytesToWrite;
        block = fat[block];
    }

    // Find free blocks to use for new file
    std::vector<uint16_t> freeBlocks;
    int bytesLeftToWrite = fileData.size();
    int bytesWritten = 0;
    while (bytesLeftToWrite > 0)
    {
        int freeBlock = -1;
        int number_of_blocks = disk.get_no_blocks();

        for (int i = 0; i < number_of_blocks; i++)
        {
            if (fat[i] == FAT_FREE)
            {
                freeBlock = i;
                break;
            }
        }

        // Return if no free blocks are avaiable (Directory is full)
        if (freeBlock == -1)
        {
            return 7;
        }

        freeBlocks.push_back(freeBlock);

        uint8_t blockBuffer[BLOCK_SIZE]{};
        int bytesToWrite = std::min(bytesLeftToWrite, BLOCK_SIZE);
        memcpy(blockBuffer, fileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, blockBuffer) != 0)
        {
            return 8;
        }

        bytesWritten += bytesToWrite;
        bytesLeftToWrite -= bytesToWrite;
    }

    // Update the FAT by writing the blocks into the new file's chain
    for (int i = 0; i < freeBlocks.size(); i++)
    {
        if (i + 1 < freeBlocks.size())
        {
            fat[freeBlocks[i]] = freeBlocks[i + 1];
        }
        else 
        {
            fat[freeBlocks[i]] = FAT_EOF;
        }
    }
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 9;
    }

    // Create a new entry in the root directory for the file
    dir_entry newFile{};    
    std::string newName;
       
    if (destFile && destFile->type == TYPE_DIR )
    {
       newName = sourcepath;
    }
    else
    {
        newName = destpath;
    }

    if (newName == "..") 
    {
        //std::cout << "Error: Invalid filename" << std::endl;
        return 10;
        
    }

    strncpy(newFile.file_name, newName.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';  // Add null terminator to end of filename
    newFile.size = fileData.size(); // Assign data size to new file entry from string
    newFile.first_blk = freeBlocks.empty() ? 0xFFFF : freeBlocks[0]; // Fail safe if the file has no data in it
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE;

    if (destFile && destFile->type == TYPE_DIR)
    {
        uint8_t subDirBuffer[BLOCK_SIZE];
        if (disk.read(destFile->first_blk, subDirBuffer) != 0)
        {
            return 12;
        }

        dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);

        // Check for duplicate files in sub directory
        for (int i = 0; i < number_of_entries; i++)
        {
            if (subDir_entries[i].file_name[0] != '\0' && strcmp(subDir_entries[i].file_name, newFile.file_name) == 0)
            {
                    //std::cout << "ERROR: file with same name already exists in subdirectory" << std::endl;
                    return 14;  
            }
        }

        // Look for an empty slot for the new file in sub directory
        bool freeSubDirEntry = false;
        for (int i = 0; i < number_of_entries; i++)
        {
            if (subDir_entries[i].file_name[0] == '\0' && strcmp(subDir_entries[i].file_name, "..") != 0)
            {
                subDir_entries[i] = newFile;
                freeSubDirEntry = true;
                break;
            }
        }

        if (!freeSubDirEntry)
        {
            //std::cout << "ERROR: sub directory is full." << std::endl;
            return 15;
        }

        // Update sub directory with new file
        if (disk.write(destFile->first_blk, subDirBuffer) != 0)
        {
            return 16;
        }
    }
    else
    {
        // Check for duplicate filename in current directory
        for (int i = 0; i < number_of_entries; i++)
        {
            if (dir_entries[i].file_name[0] != '\0' &&
                strcmp(dir_entries[i].file_name, newFile.file_name) == 0)
            {
                // File with same name already exists in current directory
                return 17;
            }
        }
       
        // Find free entry in current directory for new file
        bool freeCurDirEntry = false;
        for (int i = 0; i < number_of_entries; i++)
        {
            if (dir_entries[i].file_name[0] == '\0' && strcmp(dir_entries[i].file_name, "..") != 0)
            {
              
                dir_entries[i] = newFile;
                freeCurDirEntry = true;
                break;
        
            }
        }
        
        if (!freeCurDirEntry)
        {
            //std::cout << "ERROR: current directory is full." << std::endl;
            return 18;
        }

        // Update Current Directory with new file
        if (disk.write(currentDirectory, dirBuffer) != 0)
        {
            return 19; 
        }
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

    // Check max length for file name
    if (destpath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 1;
    }

    // Read in the root directory
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 2;
    }

    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    dir_entry* sourceFile = nullptr;
    dir_entry* destFile = nullptr;

     int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < number_of_entries; i++)
    {
        if (dir_entries[i].file_name[0] != '\0')
        {
            // Find the <sourcepath> in directory
            if (strcmp(dir_entries[i].file_name, sourcepath.c_str()) == 0)
            {
                sourceFile = &dir_entries[i];
            }

            // Check if <destpath> already exists in directory
            if (strcmp(dir_entries[i].file_name, destpath.c_str()) == 0)
            {
                destFile = &dir_entries[i];
            }
        }
    }
    if (!sourceFile)
    {
        //std::cout << "ERROR: sourcefile was not found" << std::endl;
        return 2;
    }

    // Move source file to existing sub directory
    if (destFile && destFile->type == TYPE_DIR)
    {
        uint8_t subDirBuffer[BLOCK_SIZE];
        if (disk.read(destFile->first_blk, subDirBuffer) != 0)
        {
            return 4;
        }

        dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);

        bool freeSubDirEntry = false;
        for (int i = 0; i < number_of_entries; i++)
        {
            if (subDir_entries[i].file_name[0] == '\0')
            {
                subDir_entries[i] = *sourceFile;
                freeSubDirEntry = true;
                break;
            }
        }

        if (!freeSubDirEntry)
        {
            std::cout << "ERROR: sub directory is full" << std::endl;
            return 5;
        }

        memset(sourceFile, 0, sizeof(dir_entry));

        if (disk.write(destFile->first_blk, subDirBuffer) != 0)
        {
            return 6;
        }
        if (disk.write(currentDirectory, dirBuffer) != 0)
        {
            return 7;
        }

        return 0;
    }

    if (destFile)
    {
        //std::cout << "ERROR: destfile was not found << std::endl;
        return 8;
    }

    strncpy(sourceFile->file_name, destpath.c_str(), sizeof(sourceFile->file_name) - 1);
    sourceFile->file_name[sizeof(sourceFile->file_name) - 1] = '\0';

    if (disk.write(currentDirectory, dirBuffer) != 0)
    {
        return 9;        
    }

    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int
FS::rm(std::string filepath)
{
    uint8_t dirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, dirBuffer) != 0)
    {
        return 1;
    }
    
    dir_entry* dir_entries = reinterpret_cast<dir_entry*>(dirBuffer);
    dir_entry* entryToRemove = nullptr;

    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < number_of_entries; i++) 
    {
        if (dir_entries[i].file_name[0] != '\0')
        {
            if (strcmp(dir_entries[i].file_name, filepath.c_str()) == 0)
            {
                entryToRemove = &dir_entries[i];
                break;
            }
        }
    }
    if (!entryToRemove)
    {
        //std::cout << "ERROR: File not found" << std::endl;
        return 2;
    }
    
    if (entryToRemove->type == TYPE_DIR)
    {
        uint8_t subDirBuffer[BLOCK_SIZE];
        if (disk.read(entryToRemove->first_blk, subDirBuffer) != 0)
        {
            return 3;
        }

        dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);
        
        bool empty = true;
        for (int i = 0; i < number_of_entries; i++)
        {
            if (subDir_entries[i].file_name[0] != '\0')
            {
                if (strcmp(subDir_entries[i].file_name, "..") != 0)
                {
                    empty = false;
                    break;
                }
            }
        }

        if (!empty)
        {
            std::cout << "ERROR: can't remove non-empty directory" << std::endl;
            return 4;
        }
    }


    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 5;
    }

    int16_t currentBlock = static_cast<int16_t>(entryToRemove->first_blk);
 
    // Traverse the FAT chain while freeing all blocks allocated for the entry
    while (currentBlock != FAT_EOF)
    {
        int16_t nextBlock = fat[currentBlock];
        fat[currentBlock] = FAT_FREE;
        currentBlock = nextBlock;
    }

    memset(entryToRemove, 0, sizeof(dir_entry));

    if (disk.write(currentDirectory, dirBuffer) != 0)
    {
        return 6;
    }
    
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 7;
    }

    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int
FS::append(std::string filepath1, std::string filepath2)
{
    // Read root directory
    uint8_t rootBuffer[BLOCK_SIZE];
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 1;
    }
    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(rootBuffer);

    // Look for source and destination files
    dir_entry* sourceFile = nullptr;
    dir_entry* destFile = nullptr;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
    {
        if (directory_entries[i].file_name[0] != '\0') 
        {
            if (strcmp(directory_entries[i].file_name, filepath1.c_str()) == 0)
            {
                sourceFile = &directory_entries[i];
            }
    
            if (strcmp(directory_entries[i].file_name, filepath2.c_str()) == 0)
            {
                destFile = &directory_entries[i]; 
            } 
        }
    }
    if (!sourceFile) 
    {
        return 2;
    }
    if (!destFile) 
    {
        return 3;
    }

    // Load FAT into memory
    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 4;
    }

    // Read entire source file into memory
    std::string sourceFileData;
    int16_t currentBlock = static_cast<int16_t>(sourceFile->first_blk);
    int bytesLeftToRead = sourceFile->size;

    while (currentBlock != FAT_EOF && bytesLeftToRead > 0)
    {
        if (currentBlock < 0 || currentBlock >= disk.get_no_blocks())
        {
            return 5;
        }

        uint8_t blockBuffer[BLOCK_SIZE]{};
        if (disk.read(currentBlock, blockBuffer) != 0)
        {
            return 6;
        }

        int bytesToRead = std::min(bytesLeftToRead, BLOCK_SIZE);
        sourceFileData.append(reinterpret_cast<char*>(blockBuffer), bytesToRead);

        bytesLeftToRead -= bytesToRead;
        currentBlock = fat[currentBlock];
    }

    // Sourcefile is empty, nothing to append
    if (sourceFileData.empty())
    {
        return 0;
    } 

    std::vector<int16_t> destNewBlocks;
    int sourceFileBytesLeft = sourceFileData.size();
    int bytesWritten = 0;
    int number_of_blocks = disk.get_no_blocks();

    // If destination file is empty
    if (destFile->first_blk == 0xFFFF)
    {
    }
    else
    {
        // Find last block of dest
        int16_t lastBlock = destFile->first_blk;
        while (fat[lastBlock] != FAT_EOF)
        {
            lastBlock = fat[lastBlock];
        }

        // See if last block has free space
        int usedBytes = destFile->size % BLOCK_SIZE;
        if (usedBytes != 0 && sourceFileBytesLeft > 0)
        {
            uint8_t blockBuffer[BLOCK_SIZE];
            if (disk.read(lastBlock, blockBuffer) != 0)
            {
                return 8;
            }

            int freeSpace = BLOCK_SIZE - usedBytes;
            int bytesToCopy = std::min(sourceFileBytesLeft, freeSpace);

            memcpy(blockBuffer + usedBytes, sourceFileData.data() + bytesWritten, bytesToCopy);

            if (disk.write(lastBlock, blockBuffer) != 0)
            {
                return 8;
            }

            bytesWritten += bytesToCopy;
            sourceFileBytesLeft -= bytesToCopy;
        }
    }

    // Allocate NEW blocks if there’s still data left
    while (sourceFileBytesLeft > 0)
    {
        int emptyBlock = -1;
        for (int i = 0; i < number_of_blocks; i++)
        {
            if (fat[i] == FAT_FREE)
            {
                emptyBlock = i;
                break;
            }
        }
        // Directory is full, no blocks available
        if (emptyBlock == -1)
        {
            return 7; 

        } 
        destNewBlocks.push_back(emptyBlock);

        uint8_t buffer[BLOCK_SIZE]{};
        int bytesToWrite = std::min(sourceFileBytesLeft, BLOCK_SIZE);
        memcpy(buffer, sourceFileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(emptyBlock, buffer) != 0) 
        {
            return 8;
        }

        bytesWritten += bytesToWrite;
        sourceFileBytesLeft -= bytesToWrite;
    }

    // Link new blocks together
    for (int i = 0; i < destNewBlocks.size(); i++)
    {
        if (i + 1 < destNewBlocks.size())
        {
            fat[destNewBlocks[i]] = destNewBlocks[i + 1];
        }
        else
        {
            fat[destNewBlocks[i]] = FAT_EOF;
        }
    }

    //  Attach to destination FAT chain
    if (destFile->first_blk == 0xFFFF) // empty file
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

    // Write FAT and root back to disk
    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0) 
    {
        return 9;
    }
    if (disk.write(ROOT_BLOCK, rootBuffer) != 0) 
    {
        return 10;
    }

    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int
FS::mkdir(std::string dirpath)
{
     // Check max length for file name
    if (dirpath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 1;
    }

    uint8_t parentBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, parentBuffer) != 0)
    {
        return 2;
    }

    uint16_t parentBlock = currentDirectory;
    // Check if dirname already exists in the parent directory
    dir_entry* parentDir_entries = reinterpret_cast<dir_entry*>(parentBuffer);
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    for (int i = 0; i < number_of_entries; i++)
    {
        if (parentDir_entries[i].file_name[0] != '\0')
        {
            if (strcmp(parentDir_entries[i].file_name, dirpath.c_str()) == 0)
            {
                std::cout << "ERROR: \"" << dirpath << "\" directory name already exists." << std::endl;
                return 3;
            }
        }
    }

    // Check if parent has room
    bool roomInParent = false;
    for (int i = 0; i < number_of_entries; i++)
    {
        if (parentDir_entries[i].file_name[0] == '\0')
        {
            roomInParent = true;
            break;
        }
    }
    if (!roomInParent)
    {
        return 4;
    }

    if (disk.read(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 5;
    }

    // Find free block for subdir 
    int freeBlock = -1;
    for (int i = 2; i < disk.get_no_blocks(); i++)
    {
        if (fat[i] == FAT_FREE)
        {
            freeBlock = i;
            break;
        }
    }
    if (freeBlock == -1)
    {
        return 6;
    }
    fat[freeBlock] = FAT_EOF;

    uint8_t subDirBuffer[BLOCK_SIZE]{};
    dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);

    dir_entry parentEntry{};
    strncpy(parentEntry.file_name, "..", sizeof(parentEntry.file_name) - 1);
    parentEntry.file_name[sizeof(parentEntry.file_name) - 1] = '\0';
    parentEntry.type = TYPE_DIR;
    parentEntry.first_blk = currentDirectory;
    parentEntry.size = 0;
    parentEntry.access_rights = READ | WRITE | EXECUTE;
    subDir_entries[0] = parentEntry;

    if (disk.write(freeBlock, subDirBuffer) != 0)
    {
        return 7;
    }

    if (disk.write(FAT_BLOCK, reinterpret_cast<uint8_t*>(fat)) != 0)
    {
        return 8;
    }

    dir_entry subDirEntry{};
    strncpy(subDirEntry.file_name, dirpath.c_str(), sizeof(subDirEntry.file_name) - 1);
    subDirEntry.file_name[sizeof(subDirEntry.file_name) - 1] = '\0';
    subDirEntry.type = TYPE_DIR;
    subDirEntry.first_blk = freeBlock;
    subDirEntry.size = 0;
    subDirEntry.access_rights = READ | WRITE | EXECUTE;

    for (int i = 0; i < number_of_entries; i++)
    {
        if (parentDir_entries[i].file_name[0] == '\0')
        {
            parentDir_entries[i] = subDirEntry;
            break;
        }
    }

    if (disk.write(currentDirectory, parentBuffer) != 0)
    {
        return 9;
    }

    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    // Move to parent directory from a sub directory
    if (dirpath == "..")
    {
        if (currentDirectory == ROOT_BLOCK)
        {
            return 0;
        }

        uint8_t parentBuffer[BLOCK_SIZE];
        if (disk.read(currentDirectory, parentBuffer) != 0)
        {
            return 1;
        }

        dir_entry* parentDir_entries = reinterpret_cast<dir_entry*>(parentBuffer);
        currentDirectory = parentDir_entries[0].first_blk;

        //std::cout << "Going up to parent directory " << currentDirectory << std::endl;
        return 0; // Return succesfully
    }

    uint8_t subDirBuffer[BLOCK_SIZE];
    if (disk.read(currentDirectory, subDirBuffer) != 0)
    {
        return 2;
    }

    dir_entry* subDir_entries = reinterpret_cast<dir_entry*>(subDirBuffer);
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    bool directoryFound = false;
    for (int i = 0; i < number_of_entries; i++)
    {
        if (subDir_entries[i].file_name[0] != '\0')
        {
            if (strcmp(subDir_entries[i].file_name, dirpath.c_str()) == 0)
            {
                if (subDir_entries[i].type == TYPE_DIR)
                {
                    currentDirectory = subDir_entries[i].first_blk;
                    directoryFound = true;
                    break;
                }
            }
        }
    }

    if (!directoryFound)
    {
        return 3;
    }
        
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
    for (int i = filePath.size() - 1; i >= 0; i--)
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