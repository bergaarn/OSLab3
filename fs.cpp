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
    uint8_t duplicateBuffer[BLOCK_SIZE]{};
    if (disk.read(ROOT_BLOCK, duplicateBuffer) != 0)
    {
        return 2;
    }
    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(duplicateBuffer);

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
            return 4;
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
            return 5;
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
        return 6;
    } 
    dir_entry newFile{};    // Initialize struct to 0
    strncpy(newFile.file_name, filepath.c_str(), sizeof(newFile.file_name) - 1);    // Copy filepath to struct
    
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';
    newFile.size = completedText.size();    // Set size of the contents of the file to the new file
    newFile.first_blk = freeBlocks.empty() ? 0: freeBlocks[0];
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE; // Read | Write Permissions

    uint8_t rootBuffer[BLOCK_SIZE];
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 7;
    }

    // Find a free `dir_entry` slot (all zeros)
    dir_entry* entries = reinterpret_cast<dir_entry*>(rootBuffer);
    bool entryAvailable = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i)
    {
        if (entries[i].file_name[0] == '\0')
        {
            entries[i] = newFile;
            entryAvailable = true;
            break;
        }
    }

    // root directory full
    if (!entryAvailable)
    {
        return 8;
    }

    // Write back root block
    if (disk.write(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 9;
    }

    return 0;
}

// cat <filepath> reads the content of a file and prints it on the screen
int
FS::cat(std::string filepath)
{
    uint8_t rootBuffer[BLOCK_SIZE]{};
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 1;
    }

    dir_entry* directory_entries = reinterpret_cast<dir_entry*>(rootBuffer);
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

    uint16_t fileBlock = targetFile->first_blk;
    int bytesToRead = targetFile->size;

    while (fileBlock != FAT_EOF && bytesToRead > 0)
    {
        uint8_t blockBuffer[BLOCK_SIZE]{};
        if (disk.read(fileBlock, blockBuffer) != 0)
        {
            return 3;
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
    uint8_t rootBuffer[BLOCK_SIZE]{};
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 1;
    }

    // Leave room for filename
    //std::cout << std::left << std::setw(56) << "name" << std::right << "size" << std::endl;
    
    std::cout << "name\t size" << std::endl;
    
    dir_entry* rootDir_entries = reinterpret_cast<dir_entry*>(rootBuffer);
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); i++)
    {
        if (rootDir_entries[i].file_name[0] != '\0')
        {
            std::cout 
                << rootDir_entries[i].file_name << "\t "
                << rootDir_entries[i].size << std::endl;
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
    // Read in the root directory
    uint8_t rootBuffer[BLOCK_SIZE]{};
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 1;
    }

    dir_entry* rootDir_entries = reinterpret_cast<dir_entry*>(rootBuffer);
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    dir_entry* sourceFile = nullptr;
    bool destpathAlreadyExists = false;
    
    for (int i = 0; i < number_of_entries; i++)
    {
        if (rootDir_entries[i].file_name[0] != '\0')
        {
            // Find the <sourcepath> in directory
            if(strcmp(rootDir_entries[i].file_name, sourcepath.c_str()) == 0)
            {
                sourceFile = &rootDir_entries[i];
            }

            // Check if <destpath> already exists in directory
            if (strcmp(rootDir_entries[i].file_name, destpath.c_str()) == 0)
            {
                // If so return
                destpathAlreadyExists = true;
            }
        }
    }
    if (!sourceFile)
    {
        return 2;
    }
    if (destpathAlreadyExists)
    {
        //std::cout << "ERROR: destpath filename already exists" << std::endl;
        return 3;
    }

    // Extract file data from sourcefile's blocks
    std::string fileData;
    uint16_t block = sourceFile->first_blk;
    int bytesToRead = sourceFile->size;

    while (block != FAT_EOF && bytesToRead > 0)
    {
        uint8_t blockBuffer[BLOCK_SIZE]{};
        if (disk.read(block, blockBuffer) != 0)
        {
            return 4;
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
            return 5;
        }

        freeBlocks.push_back(freeBlock);

        uint8_t blockBuffer[BLOCK_SIZE]{};
        int bytesToWrite = std::min(bytesLeftToWrite, BLOCK_SIZE);
        memcpy(blockBuffer, fileData.data() + bytesWritten, bytesToWrite);

        if (disk.write(freeBlock, blockBuffer) != 0)
        {
            return 6;
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
        return 7;
    }

    // Create a new entry in the root directory for the file
    dir_entry newFile{};    
    strncpy(newFile.file_name, destpath.c_str(), sizeof(newFile.file_name) - 1);
    newFile.file_name[sizeof(newFile.file_name) - 1] = '\0';  // Add null terminator to end of filename
    newFile.size = fileData.size(); // Assign data size to new file entry from string
    newFile.first_blk = freeBlocks.empty() ? 0 : freeBlocks[0]; // Fail safe if the file has no data in it
    newFile.type = TYPE_FILE;
    newFile.access_rights = READ | WRITE;

    // find free slot in root
    bool entryAvailable = false;
    for (int i = 0; i < BLOCK_SIZE / sizeof(dir_entry); ++i)
    {
        if (rootDir_entries[i].file_name[0] == '\0')
        {
            rootDir_entries[i] = newFile;
            entryAvailable = true;
            break;
        }
    }

    // Root directory is full
    if (!entryAvailable)
    {
        return 8; // no free root entry
    }
        
    if (disk.write(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 9; 
    }

    return 0;
}

// mv <sourcepath> <destpath> renames the file <sourcepath> to the name <destpath>,
// or moves the file <sourcepath> to the directory <destpath> (if dest is a directory)
int
FS::mv(std::string sourcepath, std::string destpath)
{

    // Check max length for file name
    if (destpath.size() >= 56)
    {
        //std::cout << "Error: Filename too long (55 characters maximum)" << std::endl;
        return 1;
    }

    // Read in the root directory
    uint8_t rootBuffer[BLOCK_SIZE]{};
    if (disk.read(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 2;
    }

    dir_entry* rootDir_entries = reinterpret_cast<dir_entry*>(rootBuffer);
    int number_of_entries = BLOCK_SIZE / sizeof(dir_entry);
    dir_entry* sourceFile = nullptr;
    bool destpathAlreadyExists = false;
    
    for (int i = 0; i < number_of_entries; i++)
    {
        if (rootDir_entries[i].file_name[0] != '\0')
        {
            // Find the <sourcepath> in directory
            if (strcmp(rootDir_entries[i].file_name, sourcepath.c_str()) == 0)
            {
                sourceFile = &rootDir_entries[i];
            }

            // Check if <destpath> already exists in directory
            if (strcmp(rootDir_entries[i].file_name, destpath.c_str()) == 0)
            {
                // If so return
                destpathAlreadyExists = true;
            }
        }
    }
    if (!sourceFile)
    {
        //std::cout << "ERROR: sourcefile was not found" << std::endl;
        return 2;
    }
    if (destpathAlreadyExists)
    {
        //std::cout << "ERROR: destpath filename already exists" << std::endl;
        return 3;
    }

    strncpy(sourceFile->file_name,destpath.c_str(), sizeof(sourceFile->file_name - 1));
    sourceFile->file_name[sizeof(sourceFile->file_name) - 1] = '\0';

    if (disk.write(ROOT_BLOCK, rootBuffer) != 0)
    {
        return 4;        
    }

    return 0;
}

// rm <filepath> removes / deletes the file <filepath>
int
FS::rm(std::string filepath)
{
    std::cout << "FS::rm(" << filepath << ")\n";
    return 0;
}

// append <filepath1> <filepath2> appends the contents of file <filepath1> to
// the end of file <filepath2>. The file <filepath1> is unchanged.
int
FS::append(std::string filepath1, std::string filepath2)
{
    std::cout << "FS::append(" << filepath1 << "," << filepath2 << ")\n";
    return 0;
}

// mkdir <dirpath> creates a new sub-directory with the name <dirpath>
// in the current directory
int
FS::mkdir(std::string dirpath)
{
    std::cout << "FS::mkdir(" << dirpath << ")\n";
    return 0;
}

// cd <dirpath> changes the current (working) directory to the directory named <dirpath>
int
FS::cd(std::string dirpath)
{
    std::cout << "FS::cd(" << dirpath << ")\n";
    return 0;
}

// pwd prints the full path, i.e., from the root directory, to the current
// directory, including the currect directory name
int
FS::pwd()
{
    std::cout << "FS::pwd()\n";
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
